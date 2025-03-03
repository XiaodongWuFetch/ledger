//------------------------------------------------------------------------------
//
//   Copyright 2018-2019 Fetch.AI Limited
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
//
//------------------------------------------------------------------------------

#include "core/byte_array/encoders.hpp"
#include "core/feature_flags.hpp"
#include "core/threading.hpp"
#include "ledger/block_packer_interface.hpp"
#include "ledger/block_sink_interface.hpp"
#include "ledger/chain/block_coordinator.hpp"
#include "ledger/chain/consensus/dummy_miner.hpp"
#include "ledger/chain/main_chain.hpp"
#include "ledger/chain/transaction.hpp"
#include "ledger/consensus/stake_manager_interface.hpp"
#include "ledger/dag/dag_interface.hpp"
#include "ledger/execution_manager_interface.hpp"
#include "ledger/storage_unit/storage_unit_interface.hpp"
#include "ledger/transaction_status_cache.hpp"
#include "ledger/upow/synergetic_execution_manager.hpp"
#include "ledger/upow/synergetic_executor.hpp"
#include "telemetry/counter.hpp"
#include "telemetry/registry.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace fetch {
namespace ledger {
namespace {

using fetch::byte_array::ToBase64;

using ScheduleStatus       = fetch::ledger::ExecutionManagerInterface::ScheduleStatus;
using ExecutionState       = fetch::ledger::ExecutionManagerInterface::State;
using SynergeticExecMgrPtr = std::unique_ptr<SynergeticExecutionManagerInterface>;
using SynergeticMinerPtr   = std::unique_ptr<SynergeticMinerInterface>;
using ProverPtr            = BlockCoordinator::ProverPtr;
using DAGPtr               = std::shared_ptr<ledger::DAGInterface>;

// Constants
const std::chrono::milliseconds TX_SYNC_NOTIFY_INTERVAL{1000};
const std::chrono::milliseconds EXEC_NOTIFY_INTERVAL{500};
const std::chrono::seconds      NOTIFY_INTERVAL{10};
const std::chrono::seconds      WAIT_BEFORE_ASKING_FOR_MISSING_TX_INTERVAL{30};
const std::chrono::seconds      WAIT_FOR_TX_TIMEOUT_INTERVAL{30};
const uint32_t                  THRESHOLD_FOR_FAST_SYNCING{100u};
const std::size_t               DIGEST_LENGTH_BYTES{32};

SynergeticExecMgrPtr CreateSynergeticExecutor(core::FeatureFlags const &features, DAGPtr dag,
                                              StorageUnitInterface &storage_unit)
{
  SynergeticExecMgrPtr execution_mgr{};

  if (features.IsEnabled("synergetic"))
  {
    execution_mgr = std::make_unique<SynergeticExecutionManager>(
        dag, 1u, [&storage_unit]() { return std::make_shared<SynergeticExecutor>(storage_unit); });
  }

  return execution_mgr;
}
}  // namespace

/**
 * Construct the Block Coordinator
 *
 * @param chain The reference to the main change
 * @param execution_manager  The reference to the execution manager
 */
BlockCoordinator::BlockCoordinator(MainChain &chain, DAGPtr dag, StakeManagerPtr stake,
                                   ExecutionManagerInterface &execution_manager,
                                   StorageUnitInterface &storage_unit, BlockPackerInterface &packer,
                                   BlockSinkInterface &      block_sink,
                                   TransactionStatusCache &  status_cache,
                                   core::FeatureFlags const &features, ProverPtr const &prover,
                                   std::size_t num_lanes, std::size_t num_slices,
                                   std::size_t block_difficulty)
  : chain_{chain}
  , dag_{std::move(dag)}
  , stake_{std::move(stake)}
  , execution_manager_{execution_manager}
  , storage_unit_{storage_unit}
  , block_packer_{packer}
  , block_sink_{block_sink}
  , status_cache_{status_cache}
  , periodic_print_{NOTIFY_INTERVAL}
  , miner_{std::make_shared<consensus::DummyMiner>()}
  , last_executed_block_{GENESIS_DIGEST}
  , mining_address_{prover->identity()}
  , state_machine_{std::make_shared<StateMachine>("BlockCoordinator", State::RELOAD_STATE,
                                                  [](State state) { return ToString(state); })}
  , block_difficulty_{block_difficulty}
  , num_lanes_{num_lanes}
  , num_slices_{num_slices}
  , tx_wait_periodic_{TX_SYNC_NOTIFY_INTERVAL}
  , exec_wait_periodic_{EXEC_NOTIFY_INTERVAL}
  , syncing_periodic_{NOTIFY_INTERVAL}
  , synergetic_exec_mgr_{CreateSynergeticExecutor(features, dag, storage_unit_)}
  , reload_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_reload_state_total",
        "The total number of times in the reload state")}
  , synchronising_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_synchronising_state_total",
        "The total number of times in the synchronising state")}
  , synchronised_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_synchronised_state_total",
        "The total number of times in the synchronised state")}
  , pre_valid_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_pre_valid_state_total",
        "The total number of times in the pre validation state")}
  , wait_tx_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_wait_tx_state_total",
        "The total number of times in the wait for tx state")}
  , syn_exec_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_syn_exec_state_total",
        "The total number of times in the synergetic execution state")}
  , sch_block_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_sch_block_state_total",
        "The total number of times in the schedule block exec state")}
  , wait_exec_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_wait_exec_state_total",
        "The total number of times in the waiting for exec state")}
  , post_valid_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_post_valid_state_total",
        "The total number of times in the post validation state")}
  , pack_block_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_pack_block_state_total",
        "The total number of times in the pack new block state")}
  , new_syn_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_new_syn_state_total",
        "The total number of times in the new synergetic state")}
  , new_exec_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_new_exec_state_total",
        "The total number of times in the new synergetic exec state")}
  , new_wait_exec_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_new_wait_exec_state_total",
        "The total number of times in the new wait exec state")}
  , proof_search_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_proof_search_state_total",
        "The total number of times in the proof search state")}
  , transmit_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_transmit_state_total",
        "The total number of times in the transmit state")}
  , reset_state_count_{telemetry::Registry::Instance().CreateCounter(
        "ledger_block_coordinator_reset_state_total",
        "The total number of times in the reset state")}
{
  // configure the state machine
  // clang-format off
  state_machine_->RegisterHandler(State::RELOAD_STATE,                 this, &BlockCoordinator::OnReloadState);
  state_machine_->RegisterHandler(State::SYNCHRONISING,                this, &BlockCoordinator::OnSynchronising);
  state_machine_->RegisterHandler(State::SYNCHRONISED,                 this, &BlockCoordinator::OnSynchronised);

  // Pipe 1
  state_machine_->RegisterHandler(State::PRE_EXEC_BLOCK_VALIDATION,    this, &BlockCoordinator::OnPreExecBlockValidation);
  state_machine_->RegisterHandler(State::SYNERGETIC_EXECUTION,         this, &BlockCoordinator::OnSynergeticExecution);
  state_machine_->RegisterHandler(State::WAIT_FOR_TRANSACTIONS,        this, &BlockCoordinator::OnWaitForTransactions);
  state_machine_->RegisterHandler(State::SCHEDULE_BLOCK_EXECUTION,     this, &BlockCoordinator::OnScheduleBlockExecution);
  state_machine_->RegisterHandler(State::WAIT_FOR_EXECUTION,           this, &BlockCoordinator::OnWaitForExecution);
  state_machine_->RegisterHandler(State::POST_EXEC_BLOCK_VALIDATION,   this, &BlockCoordinator::OnPostExecBlockValidation);

  // Pipe 2
  state_machine_->RegisterHandler(State::PACK_NEW_BLOCK,               this, &BlockCoordinator::OnPackNewBlock);
  state_machine_->RegisterHandler(State::NEW_SYNERGETIC_EXECUTION,     this, &BlockCoordinator::OnNewSynergeticExecution);
  state_machine_->RegisterHandler(State::EXECUTE_NEW_BLOCK,            this, &BlockCoordinator::OnExecuteNewBlock);
  state_machine_->RegisterHandler(State::WAIT_FOR_NEW_BLOCK_EXECUTION, this, &BlockCoordinator::OnWaitForNewBlockExecution);
  state_machine_->RegisterHandler(State::PROOF_SEARCH,                 this, &BlockCoordinator::OnProofSearch);

  state_machine_->RegisterHandler(State::TRANSMIT_BLOCK,               this, &BlockCoordinator::OnTransmitBlock);
  state_machine_->RegisterHandler(State::RESET,                        this, &BlockCoordinator::OnReset);
  // clang-format on

  state_machine_->OnStateChange([this](State current, State previous) {
    if (periodic_print_.Poll())
    {
      FETCH_LOG_INFO(LOGGING_NAME, "Current state: ", ToString(current),
                     " (previous: ", ToString(previous), ")");
    }
  });

  // TODO(private issue 792): this shouldn't be here, but if it is, it locks the whole system on
  // startup. RecoverFromStartup();
}

/**
 * Force the block interval to expire causing the state machine to be able to generate a block if
 * needed
 */
void BlockCoordinator::TriggerBlockGeneration()
{
  if (mining_)
  {
    next_block_time_ = Clock::now();
  }
}

BlockCoordinator::State BlockCoordinator::OnReloadState()
{
  reload_state_count_->increment();

  // if no current block then this is the first time in the state therefore lookup the heaviest
  // block
  if (!current_block_)
  {
    current_block_ = chain_.GetHeaviestBlock();
  }

  // if we have reached genesis then this is either because we have no state to reload in the case
  // of a fresh node, or a long series of errors prevents us from reloading previous state. In
  // either case we transition to the restarting the coordination
  assert(static_cast<bool>(current_block_));
  if (GENESIS_DIGEST != current_block_->body.previous_hash)
  {
    // normal case we have found a block from which point we want to revert. Attempt to revert to it
    bool const revert_success = storage_unit_.RevertToHash(current_block_->body.merkle_hash,
                                                           current_block_->body.block_number);

    bool revert_success_dag = true;

    if (dag_)
    {
      revert_success_dag = dag_->RevertToEpoch(current_block_->body.block_number);
    }

    if (revert_success && revert_success_dag)
    {
      // we need to update the execution manager state and also our locally cached state about the
      // last block that has been executed
      execution_manager_.SetLastProcessedBlock(current_block_->body.hash);
      last_executed_block_.Set(current_block_->body.hash);
    }
  }

  return State::RESET;
}

BlockCoordinator::State BlockCoordinator::OnSynchronising()
{
  synchronising_state_count_->increment();

  // ensure that we have a current block that we are executing
  if (!current_block_)
  {
    current_block_ = chain_.GetHeaviestBlock();
  }

  if (!current_block_ || current_block_->body.hash.empty())
  {
    FETCH_LOG_ERROR(LOGGING_NAME, "Invalid heaviest block, empty block hash");

    state_machine_->Delay(std::chrono::milliseconds{500});
    return State::RESET;
  }

  // determine if extra debug is wanted or needed
  bool const extra_debug = syncing_periodic_.Poll();

  // cache some useful variables
  auto const     current_hash         = current_block_->body.hash;
  auto const     previous_hash        = current_block_->body.previous_hash;
  auto const     desired_state        = current_block_->body.merkle_hash;
  auto const     last_committed_state = storage_unit_.LastCommitHash();
  auto const     current_state        = storage_unit_.CurrentHash();
  auto const     last_processed_block = execution_manager_.LastProcessedBlock();
  uint64_t const current_dag_epoch    = dag_ ? dag_->CurrentEpoch() : 0;

#ifdef FETCH_LOG_DEBUG_ENABLED
  if (extra_debug)
  {
    FETCH_LOG_INFO(LOGGING_NAME, "Sync: Heaviest.....: 0x", chain_.GetHeaviestBlockHash().ToHex());
    FETCH_LOG_INFO(LOGGING_NAME, "Sync: Current......: 0x", current_hash.ToHex());
    FETCH_LOG_INFO(LOGGING_NAME, "Sync: Previous.....: 0x", previous_hash.ToHex());
    FETCH_LOG_INFO(LOGGING_NAME, "Sync: Desired State: 0x", desired_state.ToHex());
    FETCH_LOG_INFO(LOGGING_NAME, "Sync: Current State: 0x", current_state.ToHex());
    FETCH_LOG_INFO(LOGGING_NAME, "Sync: LCommit State: 0x", last_committed_state.ToHex());
    FETCH_LOG_INFO(LOGGING_NAME, "Sync: Last Block...: 0x", last_processed_block.ToHex());
    FETCH_LOG_INFO(LOGGING_NAME, "Sync: Last BlockInt: 0x", last_executed_block_.Get().ToHex());
    FETCH_LOG_INFO(LOGGING_NAME, "Sync: Last DAGEpoch: 0x", current_dag_epoch);
  }
#endif  // FETCH_LOG_DEBUG_ENABLED

  FETCH_UNUSED(current_dag_epoch);

  // initial condition, the last processed block is empty
  if (GENESIS_DIGEST == last_processed_block)
  {
    // start up - we need to work out which of the blocks has been executed previously

    if (GENESIS_DIGEST == previous_hash)
    {
      // once we have got back to genesis then we need to start executing from the beginning
      return State::PRE_EXEC_BLOCK_VALIDATION;
    }
    else
    {
      // look up the previous block
      auto previous_block = chain_.GetBlock(previous_hash);
      if (!previous_block)
      {
        FETCH_LOG_WARN(LOGGING_NAME, "Unable to lookup previous block: ", ToBase64(current_hash));
        return State::RESET;
      }

      // update the current block
      current_block_ = previous_block;
    }
  }
  else if (current_hash == last_processed_block)
  {
    // the block coordinator has now successfully synced with the chain of blocks.
    return State::SYNCHRONISED;
  }
  else
  {
    // normal case - we have processed at least one block

    // find the path to ancestor - retain this path if it is long for efficiency reasons.
    bool lookup_success = false;

    if (blocks_to_common_ancestor_.empty())
    {
      lookup_success = chain_.GetPathToCommonAncestor(
          blocks_to_common_ancestor_, current_hash, last_processed_block,
          COMMON_PATH_TO_ANCESTOR_LENGTH_LIMIT, MainChain::BehaviourWhenLimit::RETURN_LEAST_RECENT);
    }
    else
    {
      lookup_success = true;
    }

    if (!lookup_success)
    {
      FETCH_LOG_WARN(LOGGING_NAME,
                     "Unable to lookup common ancestor for block:", ToBase64(current_hash));
      return State::RESET;
    }

    assert(blocks_to_common_ancestor_.size() >= 2 &&
           "Expected at least two blocks from common ancestor: HEAD and current");

    auto     block_path_it = blocks_to_common_ancestor_.crbegin();
    BlockPtr common_parent = *block_path_it++;
    BlockPtr next_block    = *block_path_it++;

    if (extra_debug)
    {
      FETCH_LOG_DEBUG(LOGGING_NAME, "Sync: Common Parent: 0x", common_parent->body.hash.ToHex());
      FETCH_LOG_DEBUG(LOGGING_NAME, "Sync: Next Block...: 0x", next_block->body.hash.ToHex());

      // calculate a percentage synchronisation
      std::size_t const current_block_num = next_block->body.block_number;
      std::size_t const total_block_num   = current_block_->body.block_number;
      double const      completion =
          static_cast<double>(current_block_num * 100) / static_cast<double>(total_block_num);

      FETCH_LOG_INFO(LOGGING_NAME, "Synchronising of chain in progress. ", completion, "% (block ",
                     next_block->body.block_number, " of ", current_block_->body.block_number, ")");
    }

    // we expect that the common parent in this case will always have been processed, but this
    // should be checked
    if (!storage_unit_.HashExists(common_parent->body.merkle_hash,
                                  common_parent->body.block_number))
    {
      FETCH_LOG_ERROR(LOGGING_NAME, "Ancestor block's state hash cannot be retrieved for block: 0x",
                      current_hash.ToHex(), " number: ", common_parent->body.block_number);

      // this is a bad situation so the easiest solution is to revert back to genesis
      execution_manager_.SetLastProcessedBlock(GENESIS_DIGEST);
      if (!storage_unit_.RevertToHash(GENESIS_MERKLE_ROOT, 0))
      {
        FETCH_LOG_ERROR(LOGGING_NAME, "Unable to revert back to genesis");
      }

      if (dag_ && !dag_->RevertToEpoch(0))
      {
        FETCH_LOG_ERROR(LOGGING_NAME, "Unable to revert DAG back to genesis!");
      }

      // delay the state machine in these error cases, to allow the network to catch up if the issue
      // is network related and if nothing else restrict logs being spammed
      state_machine_->Delay(std::chrono::seconds{5});

      return State::RESET;
    }

    // revert the storage back to the known state
    if (!storage_unit_.RevertToHash(common_parent->body.merkle_hash,
                                    common_parent->body.block_number))
    {
      FETCH_LOG_ERROR(LOGGING_NAME, "Unable to restore state for block", ToBase64(current_hash));

      // delay the state machine in these error cases, to allow the network to catch up if the issue
      // is network related and if nothing else restrict logs being spammed
      state_machine_->Delay(std::chrono::seconds{5});

      return State::RESET;
    }

    if (dag_ && !dag_->RevertToEpoch(common_parent->body.block_number))
    {
      FETCH_LOG_ERROR(LOGGING_NAME,
                      "Failed to revert dag to block: ", common_parent->body.block_number);
      state_machine_->Delay(std::chrono::seconds{5});
      return State::RESET;
    }

    // update the current block and begin scheduling
    current_block_ = next_block;

    blocks_to_common_ancestor_.pop_back();

    if (blocks_to_common_ancestor_.size() < THRESHOLD_FOR_FAST_SYNCING)
    {
      blocks_to_common_ancestor_.clear();
    }

    return State::PRE_EXEC_BLOCK_VALIDATION;
  }

  return State::SYNCHRONISING;
}

BlockCoordinator::State BlockCoordinator::OnSynchronised(State current, State previous)
{
  synchronised_state_count_->increment();

  FETCH_UNUSED(current);

  // ensure the periodic print is not trigger once we have synced
  syncing_periodic_.Reset();

  // if we have detected a change in the chain then we need to re-evaluate the chain
  if (chain_.GetHeaviestBlockHash() != current_block_->body.hash)
  {
    return State::RESET;
  }
  else if (mining_ && mining_enabled_ && (Clock::now() >= next_block_time_))
  {
    // POS: Additional check, are we able to mine?
    if (stake_)
    {
      if (!stake_->ShouldGenerateBlock(*current_block_, mining_address_))
      {
        // delay the invocation of this state machine
        state_machine_->Delay(std::chrono::milliseconds{100});

        // TODO(issue 1245): Refactor this stage, getting a little complicated
        return State::SYNCHRONISED;
      }
    }

    // create a new block
    next_block_                     = std::make_unique<Block>();
    next_block_->body.previous_hash = current_block_->body.hash;
    next_block_->body.block_number  = current_block_->body.block_number + 1;
    next_block_->body.miner         = mining_address_;

    if (stake_)
    {
      next_block_->weight = stake_->GetBlockGenerationWeight(*current_block_, mining_address_);
    }

    // Attach current DAG state
    if (dag_)
    {
      next_block_->body.dag_epoch = dag_->CreateEpoch(next_block_->body.block_number);
    }

    // ensure the difficulty is correctly set
    next_block_->proof.SetTarget(block_difficulty_);

    // discard the current block (we are making a new one)
    current_block_.reset();

    // trigger packing state
    return State::NEW_SYNERGETIC_EXECUTION;
  }
  else if (State::SYNCHRONISING == previous)
  {
    FETCH_LOG_INFO(LOGGING_NAME, "Chain Sync complete on 0x", current_block_->body.hash.ToHex(),
                   " (block: ", current_block_->body.block_number, " prev: 0x",
                   current_block_->body.previous_hash.ToHex(), ")");
  }
  else
  {
    // delay the invocation of this state machine
    state_machine_->Delay(std::chrono::milliseconds{100});
  }

  return State::SYNCHRONISED;
}

BlockCoordinator::State BlockCoordinator::OnPreExecBlockValidation()
{
  pre_valid_state_count_->increment();

  bool const is_genesis = current_block_->body.previous_hash == GENESIS_DIGEST;

  auto fail{[this](char const *reason) {
    FETCH_LOG_WARN(LOGGING_NAME, "Block validation failed: ", reason, " (",
                   ToBase64(current_block_->body.hash), ')');
    (void)reason;
    chain_.RemoveBlock(current_block_->body.hash);
    return State::RESET;
  }};

  // Check: Ensure that we have a previous block

  if (!is_genesis)
  {
    BlockPtr previous = chain_.GetBlock(current_block_->body.previous_hash);
    if (!previous)
    {
      return fail("No previous block in chain");
    }

    // Check that the weight as given by the proof is correct
    if (stake_)
    {
      if (!stake_->ValidMinerForBlock(*previous, current_block_->body.miner))
      {
        return fail("Block signed by miner deemed invalid by the staking mechanism");
      }

      if (current_block_->weight !=
          stake_->GetBlockGenerationWeight(*previous, current_block_->body.miner))
      {
        return fail("Incorrect stake weight found for block");
      }
    }

    // Check: Ensure the block number is continuous
    uint64_t const expected_block_number = previous->body.block_number + 1u;
    if (expected_block_number != current_block_->body.block_number)
    {
      return fail("Block number mismatch");
    }

    // Check: Ensure the number of lanes is correct
    if (num_lanes_ != (1u << current_block_->body.log2_num_lanes))
    {
      return fail("Lane count mismatch");
    }

    // Check: Ensure the number of slices is correct
    if (num_slices_ != current_block_->body.slices.size())
    {
      return fail("Slice count mismatch");
    }
  }

  // Check: Ensure the digests are the correct size
  if (DIGEST_LENGTH_BYTES != current_block_->body.previous_hash.size())
  {
    return fail("Previous block hash size mismatch");
  }

  // Validating DAG hashes
  if ((!is_genesis) && synergetic_exec_mgr_)
  {
    BlockPtr previous_block = chain_.GetBlock(current_block_->body.previous_hash);

    // All work is identified on the latest DAG segment and prepared in a queue
    auto const result = synergetic_exec_mgr_->PrepareWorkQueue(*current_block_, *previous_block);
    if (SynExecStatus ::SUCCESS != result)
    {
      FETCH_LOG_WARN(LOGGING_NAME, "Block certifies work that possibly is malicious (",
                     ToBase64(current_block_->body.hash), ")");
      chain_.RemoveBlock(current_block_->body.hash);

      return State::RESET;
    }
  }

  // reset the tx wait period
  tx_wait_periodic_.Reset();

  // All the checks pass
  return State::WAIT_FOR_TRANSACTIONS;
}

BlockCoordinator::State BlockCoordinator::OnSynergeticExecution()
{
  syn_exec_state_count_->count();

  bool const is_genesis = current_block_->body.previous_hash == GENESIS_DIGEST;

  // Executing synergetic work
  if ((!is_genesis) && synergetic_exec_mgr_)
  {
    // lookup the previous block
    auto const previous_block = chain_.GetBlock(current_block_->body.previous_hash);
    if (!previous_block)
    {
      FETCH_LOG_WARN(LOGGING_NAME, "Failed to lookup previous block");
      return State::RESET;
    }

    // prepare the work queue
    auto const status = synergetic_exec_mgr_->PrepareWorkQueue(*current_block_, *previous_block);
    if (SynExecStatus::SUCCESS != status)
    {
      FETCH_LOG_WARN(LOGGING_NAME,
                     "Error preparing synergetic work queue: ", ledger::ToString(status));
      return State::RESET;
    }

    if (!synergetic_exec_mgr_->ValidateWorkAndUpdateState(current_block_->body.block_number,
                                                          num_lanes_))
    {
      FETCH_LOG_WARN(LOGGING_NAME, "Work did not execute (", ToBase64(current_block_->body.hash),
                     ")");
      chain_.RemoveBlock(current_block_->body.hash);

      return State::RESET;
    }
  }

  return State::SCHEDULE_BLOCK_EXECUTION;
}

BlockCoordinator::State BlockCoordinator::OnWaitForTransactions(State current, State previous)
{
  wait_tx_state_count_->increment();

  if (previous == current)
  {
    if (have_asked_for_missing_txs_)
    {
      // FSM is stuck waiting for transactions - has timeout elapsed?
      if (wait_for_tx_timeout_.HasExpired())
      {
        // Assume block was invalid and discard it
        chain_.RemoveBlock(current_block_->body.hash);

        return State::RESET;
      }
    }
    else
    {
      if (wait_before_asking_for_missing_tx_.HasExpired())
      {
        storage_unit_.IssueCallForMissingTxs(*pending_txs_);
        have_asked_for_missing_txs_ = true;
        wait_for_tx_timeout_.Restart(WAIT_FOR_TX_TIMEOUT_INTERVAL);
      }
    }
  }
  else
  {
    // Only just started waiting for transactions - reset countdown to issuing request to peers
    wait_before_asking_for_missing_tx_.Restart(WAIT_BEFORE_ASKING_FOR_MISSING_TX_INTERVAL);
    have_asked_for_missing_txs_ = false;
  }

  // TODO(HUT): this might need to check that storage has whatever this dag epoch needs wrt
  // contracts.
  bool dag_is_ready{true};

  if (dag_)
  {
    // This combines waiting until all dag nodes are in the epoch and epoch validation (well formed
    // dag)
    dag_is_ready = dag_->SatisfyEpoch(current_block_->body.dag_epoch);
  }

  // if the transaction digests have not been cached then do this now
  if (!pending_txs_)
  {
    pending_txs_ = std::make_unique<DigestSet>();

    for (auto const &slice : current_block_->body.slices)
    {
      for (auto const &tx : slice)
      {
        pending_txs_->insert(tx.digest());
      }
    }
  }

  // evaluate if the transactions have arrived
  auto it = pending_txs_->begin();
  while (it != pending_txs_->end())
  {
    if (storage_unit_.HasTransaction(*it))
    {
      // success - remove this element from the set
      it = pending_txs_->erase(it);
    }
    else
    {
      // otherwise advance on to the next one
      ++it;
    }
  }

  // once all the transactions are present we can then move to scheduling the block. This makes life
  // much easier all around
  if (pending_txs_->empty() && dag_is_ready)
  {
    FETCH_LOG_DEBUG(LOGGING_NAME, "All transactions have been synchronised!");

    // clear the pending transaction set
    pending_txs_.reset();

    return State::SYNERGETIC_EXECUTION;
  }
  else
  {
    // status debug
    if (tx_wait_periodic_.Poll())
    {
      FETCH_LOG_INFO(LOGGING_NAME, "Waiting for ", pending_txs_->size(), " transactions to sync");
    }

    if (!dag_is_ready)
    {
      FETCH_LOG_INFO(LOGGING_NAME, "Waiting for DAG to sync");
    }

    // signal the the next execution of the state machine should be much later in the future
    state_machine_->Delay(std::chrono::milliseconds{200});
  }

  return State::WAIT_FOR_TRANSACTIONS;
}

BlockCoordinator::State BlockCoordinator::OnScheduleBlockExecution()
{
  sch_block_state_count_->increment();

  State next_state{State::RESET};

  // schedule the current block for execution
  if (ScheduleCurrentBlock())
  {
    exec_wait_periodic_.Reset();

    next_state = State::WAIT_FOR_EXECUTION;
  }

  return next_state;
}

BlockCoordinator::State BlockCoordinator::OnWaitForExecution()
{
  wait_exec_state_count_->increment();

  State next_state{State::WAIT_FOR_EXECUTION};

  auto const status = QueryExecutorStatus();

  switch (status)
  {
  case ExecutionStatus::IDLE:
    next_state = State::POST_EXEC_BLOCK_VALIDATION;
    break;

  case ExecutionStatus::RUNNING:

    if (exec_wait_periodic_.Poll())
    {
      FETCH_LOG_INFO(LOGGING_NAME, "Waiting for execution to complete for block: ",
                     current_block_->body.hash.ToBase64());
    }

    // signal that the next execution should not happen immediately
    state_machine_->Delay(std::chrono::milliseconds{20});
    break;

  case ExecutionStatus::STALLED:
  case ExecutionStatus::ERROR:
    next_state = State::RESET;
    break;
  }

  return next_state;
}

BlockCoordinator::State BlockCoordinator::OnPostExecBlockValidation()
{
  post_valid_state_count_->increment();

  // Check: Ensure the merkle hash is correct for this block
  auto const state_hash = storage_unit_.CurrentHash();

  bool invalid_block{false};
  if (GENESIS_DIGEST != current_block_->body.previous_hash)
  {
    if (state_hash != current_block_->body.merkle_hash)
    {
      FETCH_LOG_WARN(LOGGING_NAME, "Block validation failed: Merkle hash mismatch (block: 0x",
                     current_block_->body.hash.ToHex(), " expected: 0x",
                     current_block_->body.merkle_hash.ToHex(), " actual: 0x", state_hash.ToHex(),
                     ")");

      // signal the block is invalid
      invalid_block = true;
    }
    else
    {
      FETCH_LOG_DEBUG(LOGGING_NAME, "Block validation great success: (block: 0x",
                      current_block_->body.hash.ToHex(), " expected: 0x",
                      current_block_->body.merkle_hash.ToHex(), " actual: 0x", state_hash.ToHex(),
                      ")");
    }
  }

  // After the checks have been completed, if the validation has failed, the system needs to recover
  if (invalid_block)
  {
    bool revert_successful{false};

    // we need to restore back to the previous block
    BlockPtr previous_block = chain_.GetBlock(current_block_->body.previous_hash);
    if (previous_block)
    {
      revert_successful = dag_->RevertToEpoch(previous_block->body.block_number);

      // signal the storage engine to make these changes
      if (storage_unit_.RevertToHash(previous_block->body.merkle_hash,
                                     previous_block->body.block_number) &&
          revert_successful)
      {
        execution_manager_.SetLastProcessedBlock(previous_block->body.hash);
        revert_successful = true;
      }
    }

    // if the revert has gone wrong, we need to initiate a complete re-sync
    if (!revert_successful)
    {
      if (dag_)
      {
        dag_->RevertToEpoch(0);
      }
      storage_unit_.RevertToHash(GENESIS_MERKLE_ROOT, 0);
      execution_manager_.SetLastProcessedBlock(GENESIS_DIGEST);
    }

    // finally mark the block as invalid and purge it from the chain
    chain_.RemoveBlock(current_block_->body.hash);
  }
  else
  {
    // mark all the transactions as been executed
    UpdateTxStatus(*current_block_);

    // Commit this state
    storage_unit_.Commit(current_block_->body.block_number);

    // Notify the DAG of this epoch
    if (dag_)
    {
      dag_->CommitEpoch(current_block_->body.dag_epoch);
    }

    // signal the last block that has been executed
    last_executed_block_.Set(current_block_->body.hash);
  }

  return State::RESET;
}

BlockCoordinator::State BlockCoordinator::OnPackNewBlock()
{
  pack_block_state_count_->increment();

  State next_state{State::RESET};

  try
  {
    // call the block packer
    block_packer_.GenerateBlock(*next_block_, num_lanes_, num_slices_, chain_);

    // update our desired next block time
    UpdateNextBlockTime();

    // trigger the execution of the block
    next_state = State::EXECUTE_NEW_BLOCK;
  }
  catch (std::exception const &ex)
  {
    FETCH_LOG_ERROR(LOGGING_NAME, "Error generated performing block packing: ", ex.what());
  }

  return next_state;
}

BlockCoordinator::State BlockCoordinator::OnNewSynergeticExecution()
{
  new_syn_state_count_->increment();

  if (synergetic_exec_mgr_ && dag_)
  {
    // lookup the previous block
    BlockPtr previous_block = chain_.GetBlock(next_block_->body.previous_hash);

    // prepare the work queue
    auto const status = synergetic_exec_mgr_->PrepareWorkQueue(*next_block_, *previous_block);
    if (SynExecStatus::SUCCESS != status)
    {
      FETCH_LOG_WARN(LOGGING_NAME,
                     "Error preparing synergetic work queue: ", ledger::ToString(status));
      return State::RESET;
    }

    if (!synergetic_exec_mgr_->ValidateWorkAndUpdateState(next_block_->body.block_number,
                                                          num_lanes_))
    {
      FETCH_LOG_WARN(LOGGING_NAME, "Failed to valid work queue");

      return State::RESET;
    }
  }

  return State::PACK_NEW_BLOCK;
}

BlockCoordinator::State BlockCoordinator::OnExecuteNewBlock()
{
  new_exec_state_count_->increment();

  State next_state{State::RESET};

  // schedule the current block for execution
  if (ScheduleNextBlock())
  {
    exec_wait_periodic_.Reset();

    next_state = State::WAIT_FOR_NEW_BLOCK_EXECUTION;
  }

  return next_state;
}

BlockCoordinator::State BlockCoordinator::OnWaitForNewBlockExecution()
{
  new_wait_exec_state_count_->increment();

  State next_state{State::WAIT_FOR_NEW_BLOCK_EXECUTION};

  auto const status = QueryExecutorStatus();
  switch (status)
  {
  case ExecutionStatus::IDLE:
  {
    // update the current block with the desired hash
    next_block_->body.merkle_hash = storage_unit_.CurrentHash();

    FETCH_LOG_DEBUG(LOGGING_NAME, "Merkle Hash: ", ToBase64(next_block_->body.merkle_hash));

    // Commit the state generated by this block
    storage_unit_.Commit(next_block_->body.block_number);

    // Notify the DAG of this epoch
    if (dag_)
    {
      dag_->CommitEpoch(next_block_->body.dag_epoch);
    }

    next_state = State::PROOF_SEARCH;
    break;
  }

  case ExecutionStatus::RUNNING:
    if (exec_wait_periodic_.Poll())
    {
      FETCH_LOG_WARN(LOGGING_NAME, "Waiting for new block execution (following: ",
                     next_block_->body.previous_hash.ToBase64(), ")");
    }

    // signal that the next execution should not happen immediately
    state_machine_->Delay(std::chrono::milliseconds{20});
    break;

  case ExecutionStatus::STALLED:
  case ExecutionStatus::ERROR:
    next_state = State::RESET;
    break;
  }

  return next_state;
}

BlockCoordinator::State BlockCoordinator::OnProofSearch()
{
  proof_search_state_count_->increment();

  State next_state{State::PROOF_SEARCH};

  if (miner_->Mine(*next_block_, 100))  // TODO(unknown): what is this hard-coded number?
  {
    // update the digest
    next_block_->UpdateDigest();

    FETCH_LOG_DEBUG(LOGGING_NAME, "New Block Hash: 0x", next_block_->body.hash.ToHex());

    // this step is needed because the execution manager is actually unaware of the actual last
    // block that is executed because the merkle hash was not known at this point.
    execution_manager_.SetLastProcessedBlock(next_block_->body.hash);

    // the block is now fully formed it can be sent across the network
    next_state = State::TRANSMIT_BLOCK;
  }

  return next_state;
}

BlockCoordinator::State BlockCoordinator::OnTransmitBlock()
{
  transmit_state_count_->increment();

  try
  {
    // ensure that the main chain is aware of the block
    if (BlockStatus::ADDED == chain_.AddBlock(*next_block_))
    {
      FETCH_LOG_INFO(LOGGING_NAME, "Broadcasting new block: 0x", next_block_->body.hash.ToHex(),
                     " txs: ", next_block_->GetTransactionCount(),
                     " number: ", next_block_->body.block_number);

      // mark this blocks transactions as being executed
      UpdateTxStatus(*next_block_);

      // signal the last block that has been executed
      last_executed_block_.Set(next_block_->body.hash);

      // dispatch the block that has been generated
      block_sink_.OnBlock(*next_block_);
    }
  }
  catch (std::exception const &ex)
  {
    FETCH_LOG_WARN(LOGGING_NAME, "Error transmitting verified block: ", ex.what());
  }

  return State::RESET;
}

BlockCoordinator::State BlockCoordinator::OnReset()
{
  reset_state_count_->increment();

  // trigger stake updates at the end of the block lifecycle
  if (stake_)
  {
    if (next_block_)
    {
      stake_->UpdateCurrentBlock(*next_block_);
    }
    else if (current_block_)
    {
      stake_->UpdateCurrentBlock(*current_block_);
    }
  }

  current_block_.reset();
  next_block_.reset();
  pending_txs_.reset();
  blocks_to_common_ancestor_.clear();

  // we should update the next block time
  UpdateNextBlockTime();

  return State::SYNCHRONISING;
}

bool BlockCoordinator::ScheduleCurrentBlock()
{
  bool success{false};

  // sanity check - ensure there is a block to execute
  if (current_block_)
  {
    success = ScheduleBlock(*current_block_);
  }
  else
  {
    FETCH_LOG_ERROR(LOGGING_NAME, "Unable to execute empty current block");
  }

  return success;
}

bool BlockCoordinator::ScheduleNextBlock()
{
  bool success{false};

  if (next_block_)
  {
    success = ScheduleBlock(*next_block_);
  }
  else
  {
    FETCH_LOG_ERROR(LOGGING_NAME, "Unable to execute empty next block");
  }

  return success;
}

bool BlockCoordinator::ScheduleBlock(Block const &block)
{
  bool success{false};

  FETCH_LOG_DEBUG(LOGGING_NAME, "Attempting exec on block: 0x", block.body.hash.ToHex());

  // instruct the execution manager to execute the current block
  auto const execution_status = execution_manager_.Execute(block.body);

  if (execution_status == ScheduleStatus::SCHEDULED)
  {
    // signal success
    success = true;
  }
  else
  {
    FETCH_LOG_ERROR(LOGGING_NAME,
                    "Execution engine stalled. State: ", ledger::ToString(execution_status));
  }

  return success;
}

BlockCoordinator::ExecutionStatus BlockCoordinator::QueryExecutorStatus()
{
  ExecutionStatus status{ExecutionStatus::ERROR};

  // based on the state of the execution manager determine
  auto const execution_state = execution_manager_.GetState();

  // map the raw executor status into our simplified version
  switch (execution_state)
  {
  case ExecutionState::IDLE:
    status = ExecutionStatus::IDLE;
    break;

  case ExecutionState::ACTIVE:
    status = ExecutionStatus::RUNNING;
    break;

  case ExecutionState::TRANSACTIONS_UNAVAILABLE:
    status = ExecutionStatus::STALLED;
    break;

  case ExecutionState::EXECUTION_ABORTED:
  case ExecutionState::EXECUTION_FAILED:
    FETCH_LOG_WARN(LOGGING_NAME, "Execution in error state: ", ledger::ToString(execution_state));
    status = ExecutionStatus::ERROR;
    break;
  }

  return status;
}

void BlockCoordinator::UpdateNextBlockTime()
{
  next_block_time_ = Clock::now() + block_period_;
}

void BlockCoordinator::UpdateTxStatus(Block const &block)
{
  for (auto const &slice : block.body.slices)
  {
    for (auto const &tx : slice)
    {
      status_cache_.Update(tx.digest(), TransactionStatus::EXECUTED);
    }
  }
}

char const *BlockCoordinator::ToString(State state)
{
  char const *text = "Unknown";

  switch (state)
  {
  case State::RELOAD_STATE:
    text = "Reloading State";
    break;
  case State::SYNCHRONISING:
    text = "Synchronising";
    break;
  case State::SYNCHRONISED:
    text = "Synchronised";
    break;
  case State::PRE_EXEC_BLOCK_VALIDATION:
    text = "Pre Block Execution Validation";
    break;
  case State::WAIT_FOR_TRANSACTIONS:
    text = "Waiting for Transactions";
    break;
  case State::SYNERGETIC_EXECUTION:
    text = "Synergetic Execution";
    break;
  case State::SCHEDULE_BLOCK_EXECUTION:
    text = "Schedule Block Execution";
    break;
  case State::WAIT_FOR_EXECUTION:
    text = "Waiting for Block Execution";
    break;
  case State::POST_EXEC_BLOCK_VALIDATION:
    text = "Post Block Execution Validation";
    break;
  case State::PACK_NEW_BLOCK:
    text = "Pack New Block";
    break;
  case State::NEW_SYNERGETIC_EXECUTION:
    text = "New Synergetic Execution";
    break;
  case State::EXECUTE_NEW_BLOCK:
    text = "Execution New Block";
    break;
  case State::WAIT_FOR_NEW_BLOCK_EXECUTION:
    text = "Waiting for New Block Execution";
    break;
  case State::PROOF_SEARCH:
    text = "Searching for Proof";
    break;
  case State::TRANSMIT_BLOCK:
    text = "Transmitting Block";
    break;
  case State::RESET:
    text = "Reset";
    break;
  }

  return text;
}

char const *BlockCoordinator::ToString(ExecutionStatus state)
{
  char const *text = "Unknown";

  switch (state)
  {
  case ExecutionStatus::IDLE:
    text = "Idle";
    break;
  case ExecutionStatus::RUNNING:
    text = "Running";
    break;
  case ExecutionStatus::STALLED:
    text = "Stalled";
    break;
  case ExecutionStatus::ERROR:
    text = "Error";
    break;
  }

  return text;
}

void BlockCoordinator::Reset()
{
  last_executed_block_.Set(GENESIS_DIGEST);
  execution_manager_.SetLastProcessedBlock(GENESIS_DIGEST);
  chain_.Reset();
}

}  // namespace ledger
}  // namespace fetch

#ifndef PROTOCOLS_CHAIN_KEEPER_MANAGER_HPP
#define PROTOCOLS_CHAIN_KEEPER_MANAGER_HPP
#include"byte_array/referenced_byte_array.hpp"
#include"byte_array/const_byte_array.hpp"
#include"serializer/referenced_byte_array.hpp"

#include"protocols/fetch_protocols.hpp"
#include"chain/transaction.hpp"
#include"chain/block.hpp"
#include"chain/consensus/proof_of_work.hpp"
#include"protocols/chain_keeper/block.hpp"
#include"protocols/chain_keeper/transaction_manager.hpp"
#include"protocols/chain_keeper/chain_manager.hpp"

#include "service/client.hpp"
#include"service/publication_feed.hpp"
#include"mutex.hpp"
#include"protocols/chain_keeper/commands.hpp"

#include"protocols/swarm/entry_point.hpp"
#include"logger.hpp"

#include<map>
#include<vector>
#include<limits>
#include<stack>
#include<set>


namespace fetch
{
namespace protocols 
{


class ChainKeeperController 
{
public:

  // TODO: Get from chain manager
  // Transaction defs
  typedef fetch::chain::Transaction transaction_type;
  typedef typename transaction_type::digest_type tx_digest_type;

  // Block defs  
  typedef fetch::chain::consensus::ProofOfWork proof_type;
  typedef BlockBody block_body_type;
  typedef typename proof_type::header_type block_header_type;
  typedef fetch::chain::BasicBlock< block_body_type, proof_type, fetch::crypto::SHA256 > block_type;  

  // Other groups  
  typedef fetch::service::ServiceClient< fetch::network::TCPClient > client_type;
  typedef std::shared_ptr< client_type >  client_shared_ptr_type;
  
  ChainKeeperController(uint64_t const& protocol,
    network::ThreadManager *thread_manager,
    EntryPoint& details) :
    thread_manager_(thread_manager),
    details_(details),
    block_mutex_( __LINE__, __FILE__),    
    chain_keeper_friends_mutex_( __LINE__, __FILE__),
    grouping_parameter_(1),
    chain_manager_(tx_manager_)
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    fetch::logger.Debug("Entering ", __FUNCTION_NAME__);
    
    details_.configuration = EntryPoint::NODE_CHAIN_KEEPER;
    
    block_body_type genesis_body;
    block_type genesis_block;
    
    genesis_body.previous_hash = "genesis";
    genesis_body.transaction_hash = "genesis";

    genesis_block.SetBody( genesis_body );

    genesis_block.set_block_number(0);

    
    PushBlock( genesis_block );    
  }

  // TODO: Change signature to std::vector< EntryPoint >
  EntryPoint Hello(std::string host) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    fetch::logger.Debug("Exchaning group details (RPC reciever)");

    std::lock_guard< fetch::mutex::Mutex > lock( details_mutex_ );
    
    details_.configuration = EntryPoint::NODE_CHAIN_KEEPER;
    if(details_.host != host ) { 
      details_.host = host;
    }
    
    return details_;
  }
  
  block_type ExchangeHeads(block_type head_candidate) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;

    fetch::logger.Debug("Entering ", __FUNCTION_NAME__);
    fetch::logger.Debug("Sending head as response to request");
    std::lock_guard< fetch::mutex::Mutex > lock(block_mutex_);
    
    // TODO: Check which head is better
    fetch::logger.Debug("Return!");    
    return chain_manager_.head();
  }

  std::vector< block_type > RequestBlocksFrom(block_header_type next_hash, uint16_t preferred_block_count) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    fetch::logger.Debug("Entering ", __FUNCTION_NAME__);    
    std::vector< block_type > ret;

    if( preferred_block_count > 10 ) preferred_block_count = 10;    
    ret.reserve( preferred_block_count );
    
    std::lock_guard< fetch::mutex::Mutex > lock(block_mutex_);
    std::size_t i =0;
    ChainManager::chain_map_type &chains = chain_manager_.chains();
    
    while( (i< preferred_block_count) && (chains.find( next_hash ) !=chains.end() ) ) {
      auto const &block = chains[next_hash];
      ret.push_back( *block );

      next_hash = block->body().previous_hash;
      ++i;
    }    

    return ret;    
  }

  std::vector< transaction_type > GetTransactions(  ) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    return tx_manager_.LastTransactions();    
  }

  std::vector< block_type > GetLatestBlocks(  ) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    return chain_manager_.latest_blocks();    
  }
  

  bool PushTransaction( transaction_type tx ) {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    
    if(! tx.UsesGroup( details_.group, grouping_parameter_) ) {
      fetch::logger.Debug("Transaction not belonging to group");
      return false;
    }
    
    block_mutex_.lock();

    tx.UpdateDigest();
    if(! tx_manager_.AddTransaction( tx ) )
    {      
      block_mutex_.unlock();
      return false;
    }
    
    block_mutex_.unlock();
    
    fetch::logger.Warn("Verify transaction");

    return true;
  }


  block_type GetNextBlock() { // TODO: Move out.
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;

    block_body_type body;
    block_type block;
    
    block_mutex_.lock();    
    body.previous_hash = chain_manager_.head().header();

    if( !tx_manager_.has_unapplied() ) {
      body.transaction_hash =  "";
    } else {
      body.transaction_hash =  tx_manager_.NextDigest();
    }
    block_mutex_.unlock();
    
    block.SetBody( body );   
    block.set_total_weight( chain_manager_.head().total_weight() );
    block.set_block_number( chain_manager_.head().block_number() + 1 );
    
    
    return block;
    
  }
  
  void PushBlock(block_type block) {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;

    block_mutex_.lock();
    chain_manager_.AddBlock( block );
    block_mutex_.unlock();

//    VerifyState();
  }

  void ConnectTo(std::string const &host, uint16_t const &port ) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    
    // Chained tasks
    client_shared_ptr_type client;
    std::size_t i = 0;    
    while( (!client) && (i < 3) ) { // TODO: make configurable
      
      client = std::make_shared< client_type >(host, port, thread_manager_);
      auto ping_promise = client->Call(FetchProtocols::CHAIN_KEEPER, ChainKeeperRPC::PING);
      if(!ping_promise.Wait( 500 ) ) // TODO: Make configurable
      {
        fetch::logger.Debug("Server not repsonding - retrying !");
      }
      ++i;      
    }
    
    if(!client)
    {
      fetch::logger.Error("Server not repsonding - hanging up!");
      return;      
    }
    
    EntryPoint d;
    d.host = host;    
    d.port = port;
    d.http_port = -1; 
    d.group = 0; // TODO: get and check that it is right
    d.configuration = 0;  


    block_mutex_.lock();    
    block_type head_copy = chain_manager_.head();
    block_mutex_.unlock();
   
    chain_keeper_friends_mutex_.lock();
    chain_keeper_friends_.push_back(client);
    friends_details_.push_back(d);
    chain_keeper_friends_mutex_.unlock();

    fetch::logger.Debug("Requesting head exchange");    
    auto promise1 = client->Call(FetchProtocols::CHAIN_KEEPER, ChainKeeperRPC::EXCHANGE_HEADS, head_copy);    
    if(!promise1.Wait(1000) ) { //; // TODO: make configurable
      fetch::logger.Error("Failed to get head.");
      exit(-1);
      
      return;        
    }
    if( promise1.has_failed() ) {
      fetch::logger.Error("Request for head failed.");
      return;        
    }
    
    if( promise1.is_connection_closed() ) {
      fetch::logger.Error("Lost connection.");
      return;           
    }
    
    
    block_type comp_head = promise1.As< block_type >();
    fetch::logger.Debug("Done");
    
//    comp_head.meta_data() = block_meta_data_type();      
    
    PushBlock(comp_head);
  }

  void ListenTo(std::vector< EntryPoint > list) // TODO: Rename
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;

    fetch::logger.Highlight("Updating connectivity for ", details_.host, ":", details_.port);
    
    for(auto &e: list) {
      fetch::logger.Highlight("  - ", e.host, ":", e.port,", group ", e.group);      
      details_mutex_.lock();
      bool self = (e.host == details_.host) && (e.port == details_.port);
      bool same_group = (e.group == details_.group);
      details_mutex_.unlock();
      
      if(self) {
        fetch::logger.Debug("Skipping myself");        
        continue;
      }

      if(!same_group) {
        fetch::logger.Debug("Connectiong not belonging to same group");
        
        continue;
      }
      
      
      // TODO: implement max connectivity

      bool found = false;
      chain_keeper_friends_mutex_.lock();    
      for(auto &d: friends_details_)
      {
        if((d.host == e.host) &&
          (d.port == e.port )  ) {
          found = true;
          break;
        }
      }

      chain_keeper_friends_mutex_.unlock();
      if(!found)
      {
        ConnectTo(e.host, e.port); 
      }
    }
    

  }

  void SetGroupNumber(uint32_t group, uint32_t total_groups) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;

    fetch::logger.Debug("Setting group numbers: ", group, " ", total_groups);    
    grouping_parameter_ = total_groups;
    details_.group = group;

    block_mutex_.lock();
    tx_manager_.set_group( group );
    chain_manager_.set_group( group );    
    block_mutex_.unlock();
  }
  
  uint32_t count_outgoing_connections() 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    
    std::lock_guard< fetch::mutex::Mutex > lock( chain_keeper_friends_mutex_ );
    return chain_keeper_friends_.size();    
  }

  uint32_t group_number() 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    
    return details_.group;    
  }
  

  void with_peers_do( std::function< void( std::vector< client_shared_ptr_type > , std::vector< EntryPoint > const& ) > fnc ) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    
    chain_keeper_friends_mutex_.lock();
    fnc( chain_keeper_friends_, friends_details_ );    
    chain_keeper_friends_mutex_.unlock();
  }

  void with_peers_do( std::function< void( std::vector< client_shared_ptr_type >  ) > fnc ) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    
    chain_keeper_friends_mutex_.lock();
    fnc( chain_keeper_friends_); 
    chain_keeper_friends_mutex_.unlock();
  }
  
  void with_blocks_do( std::function< void(block_type, ChainManager::chain_map_type const& )  > fnc ) const
  {
    block_mutex_.lock();
    fnc( chain_manager_.head(), chain_manager_.chains() );    
    block_mutex_.unlock();
  }

  void with_blocks_do( std::function< void(block_type, ChainManager::chain_map_type & )  > fnc ) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    
    block_mutex_.lock();
    fnc( chain_manager_.head(), chain_manager_.chains() );    
    block_mutex_.unlock();
  }
  
  /*
  void with_transactions_do( std::function< void(std::vector< tx_digest_type >,  std::map< tx_digest_type, transaction_type >) > fnc )
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    
    block_mutex_.lock();
    fnc( incoming_, known_transactions_ );    
    block_mutex_.unlock();
  }
  */
  /*
  void with_loose_chains_do( std::function< void( std::map< uint64_t,  ChainManager::PartialChain > ) > fnc ) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;
    
    block_mutex_.lock();
    fnc( chain_manager_.loose_chains() );
    block_mutex_.unlock();
  }
  */

  std::size_t unapplied_transaction_count() const 
  {
    std::lock_guard< fetch::mutex::Mutex > lock(block_mutex_);    
    return tx_manager_.unapplied_count();
  }

  std::size_t applied_transaction_count() const 
  {
    std::lock_guard< fetch::mutex::Mutex > lock(block_mutex_);    
    return tx_manager_.applied_count();    
  }

  std::size_t transaction_count() const 
  {
    std::lock_guard< fetch::mutex::Mutex > lock(block_mutex_);
    return tx_manager_.size();        
  }

  std::size_t block_count() const 
  {
    std::lock_guard< fetch::mutex::Mutex > lock(block_mutex_);
    return chain_manager_.size();        
  }
  
  bool AddBulkTransactions(std::unordered_map< tx_digest_type, transaction_type, typename TransactionManager::hasher_type > const &new_txs ) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;    
    std::lock_guard< fetch::mutex::Mutex > lock(block_mutex_);
    return tx_manager_.AddBulkTransactions(new_txs ) ;    
  }

  bool AddBulkBlocks(std::vector< block_type > const &new_blocks ) 
  {
    LOG_STACK_TRACE_POINT_WITH_INSTANCE;    
    std::lock_guard< fetch::mutex::Mutex > lock(block_mutex_);
    return chain_manager_.AddBulkBlocks(new_blocks ) ;    
  }
  
private:
 
  network::ThreadManager *thread_manager_;    
  EntryPoint &details_;  
  mutable fetch::mutex::Mutex details_mutex_;
  
  mutable fetch::mutex::Mutex block_mutex_;




  std::vector< client_shared_ptr_type > chain_keeper_friends_;
  std::vector< EntryPoint > friends_details_;  
  fetch::mutex::Mutex chain_keeper_friends_mutex_;  

  std::atomic< uint32_t > grouping_parameter_ ;

  TransactionManager tx_manager_;
  ChainManager chain_manager_;
};


};
};

#endif
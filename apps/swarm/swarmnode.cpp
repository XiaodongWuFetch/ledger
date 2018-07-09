#include <iostream>
#include <string>
#include <time.h>
#include <unistd.h>

#include "core/commandline/params.hpp"
#include "network/details/thread_manager.hpp"
#include "network/service/protocol.hpp"
#include "network/parcels/swarm_agent_api_impl.hpp"
#include "network/parcels/swarm_agent_naive.hpp"
#include "network/parcels/swarm_parcel_node.hpp"
#include "network/protocols/parcels/swarm_parcel_protocol.hpp"
#include "network/swarm/swarm_http_interface.hpp"
#include "network/swarm/swarm_node.hpp"
#include "network/swarm/swarm_peer_location.hpp"
#include "network/swarm/swarm_random.hpp"
#include "network/swarm/swarm_service.hpp"
#include<random>

typedef unsigned int uint;

static uint32_t GetRandom()
{
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<uint32_t> dis(
      0, std::numeric_limits<uint32_t>::max());
  return dis(gen);
}

int main(int argc, const char *argv[])
{
  uint32_t rand = GetRandom();
  uint16_t portNumber = 9000+(rand&0xf);
  unsigned int id{9000+(rand&0xf)};
  unsigned int maxpeers{3};
  unsigned int idlespeed{100};
  std::string peerlist{"127.0.0.1:9006,127.0.0.1:9015"};

  fetch::commandline::Params params;

  params.description("I am a demo node, for the v1 test network.");

  params.add(id,            "id",             "Identifier number for this node.");
  params.add(portNumber,    "port",           "Which port to run on.");
  params.add(maxpeers,      "maxpeers",       "Ideally how many peers to maintain good connections to.");
  params.add(idlespeed,     "idlespeed",      "The rate, in milliseconds, of generating idle events to the Swarm Agent.");
  params.add(peerlist,      "peers",          "Comma separated list of peer locations.");

  params.Parse(argc, argv); 

  std::list<fetch::swarm::SwarmPeerLocation> peers = fetch::swarm::SwarmPeerLocation::ParsePeerListString(peerlist);

  fetch::swarm::SwarmKarmaPeer::ToGetCurrentTime([](){ return time(0); });

  std::cout << "######## " << portNumber << std::endl;
  std::cout << "######## " << id << std::endl;
  std::cout << "######## " << maxpeers << std::endl;
  auto nnCore = std::make_shared<fetch::network::NetworkNodeCore>(30, portNumber+1000, portNumber);

  std::string identifier = "node-" + std::to_string(id);

  std::string myHost = "127.0.0.1:" + std::to_string(portNumber);
  fetch::swarm::SwarmPeerLocation myHostLoc(myHost);

  auto rnd = std::make_shared<fetch::swarm::SwarmRandom>(id);

  std::shared_ptr<fetch::swarm::SwarmNode> node = std::make_shared<fetch::swarm::SwarmNode>(
                                              nnCore,
                                              identifier,
                                              maxpeers,
                                              rnd,
                                              myHost
                                                                                            );
  auto parcelNode = std::make_shared<fetch::swarm::SwarmParcelNode>(nnCore);
  auto swarmAgentApi = std::make_shared<fetch::swarm::SwarmAgentApiImpl>(myHost, idlespeed);
  auto agent = std::make_shared<fetch::swarm::SwarmAgentNaive>(swarmAgentApi, identifier, id, rnd, maxpeers);
  auto httpModule = std::make_shared<fetch::swarm::SwarmHttpModule>(node);

  nnCore -> AddModule(httpModule);

  swarmAgentApi -> ToPing([swarmAgentApi, node, parcelNode](fetch::swarm::SwarmAgentApi &unused, const std::string &host)
                          {
                            node -> Post([swarmAgentApi, node, parcelNode, host]()
                                             {
                                               try
                                                 {
                                                   auto newPeer = node -> AskPeerForPeers(host);

                                                   if (newPeer.length()) {

                                                     if (!node -> IsOwnLocation(newPeer))
                                                       {
                                                         swarmAgentApi -> DoNewPeerDiscovered(newPeer);
                                                       }
                                                   }
                                                   swarmAgentApi -> DoPingSucceeded(host);
                                                 }
                                                         catch(fetch::network::NetworkNodeCoreBaseException &x)
                                                           {
                                                             cerr << " 3CAUGHT NetworkNodeCoreExceptionBase" << x.what()<< endl;
                                                             swarmAgentApi -> DoPingFailed(host);
                                                           }
                                               catch(fetch::serializers::SerializableException &x)
                                                 {
                                                   cerr << " 1CAUGHT fetch::serializers::SerializableException:" << x.what() << endl;
                                                   swarmAgentApi -> DoPingFailed(host);
                                                 }
                                               catch(fetch::swarm::SwarmException &x)
                                                 {
                                                   cerr << " 2CAUGHT SwarmException" << x.what()<< endl;
                                                   swarmAgentApi -> DoPingFailed(host);
                                                 }
                                             });
                          });

  swarmAgentApi -> ToDiscoverBlocks([swarmAgentApi, node, parcelNode](const std::string &host, unsigned int count)
                                    {
                                      node ->Post([swarmAgentApi, node, parcelNode, host, count]()
                                                       {
                                                         try
                                                           {
                                                             auto blockids = parcelNode -> AskPeerForParcelIds(host, "block", count);

                                                             for(auto &blockid : blockids)
                                                               {
                                                                 if (!parcelNode -> HasParcel("block", blockid))
                                                                   {
                                                                     swarmAgentApi -> DoNewBlockIdFound(host, blockid);
                                                                   }
                                                                 else
                                                                   {
                                                                     swarmAgentApi -> DoBlockIdRepeated(host, blockid);
                                                                   }
                                                               }
                                                           }
                                                         catch(fetch::network::NetworkNodeCoreBaseException &x)
                                                           {
                                                             cerr << " 3CAUGHT NetworkNodeCoreExceptionBase" << x.what()<< endl;
                                                             swarmAgentApi -> DoPingFailed(host);
                                                           }
                                                         catch(fetch::serializers::SerializableException &x)
                                                           {
                                                             cerr << " 3CAUGHT fetch::serializers::SerializableException" << x.what()<< endl;
                                                             swarmAgentApi -> DoPingFailed(host);
                                                           }
                                                         catch(fetch::swarm::SwarmException &x)
                                                           {
                                                             cerr << " 4CAUGHT SwarmException" << x.what()<< endl;
                                                             swarmAgentApi -> DoPingFailed(host);
                                                           }
                                                       });
                                    });

  swarmAgentApi -> ToGetBlock([swarmAgentApi, node, parcelNode](const std::string &host, const std::string &blockid)
                              {
                                node -> Post([swarmAgentApi, node, parcelNode, host, blockid]()
                                                 {
                                                   try
                                                     {
                                                       auto data = parcelNode -> AskPeerForParcelData(host, "block", blockid);

                                                       auto parcel = std::make_shared<fetch::swarm::SwarmParcel>("block", data);
                                                       if (parcel -> GetName() != blockid)
                                                         {
                                                           swarmAgentApi -> VerifyBlock(blockid, false);
                                                         }
                                                       else
                                                         {
                                                           if (!parcelNode -> HasParcel( "block", blockid))
                                                             {
                                                               parcelNode -> StoreParcel(parcel);
                                                               swarmAgentApi -> DoNewBlockAvailable(host, blockid);
                                                             }
                                                         }
                                                     }
                                                         catch(fetch::network::NetworkNodeCoreBaseException &x)
                                                           {
                                                             cerr << " 3CAUGHT NetworkNodeCoreExceptionBase" << x.what()<< endl;
                                                             swarmAgentApi -> DoPingFailed(host);
                                                           }
                                                   catch(fetch::serializers::SerializableException &x)
                                                     {
                                                       cerr << " 5CAUGHT fetch::serializers::SerializableException" << x.what()<< endl;
                                                       swarmAgentApi -> DoPingFailed(host);
                                                     }
                                                   catch(fetch::swarm::SwarmException &x)
                                                     {
                                                       cerr << " 6CAUGHT SwarmException" << x.what()<< endl;
                                                       swarmAgentApi -> DoPingFailed(host);
                                                     }
                                                 });
                              });
  swarmAgentApi -> ToGetKarma([node](const std::string &host)
                              {
                                return node -> GetKarma(host);
                              });
  swarmAgentApi -> ToAddKarma([node](const std::string &host, double amount)
                              {
                                node -> AddOrUpdate(host, amount);
                              });
  swarmAgentApi -> ToAddKarmaMax([node](const std::string &host, double amount, double limit)
                                 {
                                   if (node -> GetKarma(host) < limit)
                                     {
                                       node -> AddOrUpdate(host, amount);
                                     }
                                 });
  swarmAgentApi -> ToGetPeers([swarmAgentApi, node, parcelNode](unsigned int count, double minKarma)
                              {
                                auto karmaPeers = node -> GetBestPeers(count, minKarma);
                                std::list<std::string> results;
                                for(auto &peer: karmaPeers)
                                  {
                                    results.push_back(peer.GetLocation().AsString());
                                  }
                                if (results.empty())
                                  {
                                    swarmAgentApi -> DoPeerless();
                                  }
                                return results;
                              });

  swarmAgentApi -> ToQueryBlock([swarmAgentApi, node, parcelNode] (const std::string &id)
                                {
                                  if (!parcelNode -> HasParcel("block", id))
                                    {
                                      return std::string("<NO PARCEL>");
                                    }
                                  return parcelNode -> GetParcel("block", id) -> GetData();
                                });

  swarmAgentApi -> Start();


  for(auto &peer : peers)
    {
      agent -> addInitialPeer(peer.AsString());
    }

  nnCore -> Start();

  int dummy;

  std::cout << "press any key to quit" << std::endl;
  std::cin >> dummy;

  nnCore -> Stop();

}

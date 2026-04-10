#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/sred-queue-disc-mod.h"
#include "ns3/traffic-control-module.h"
#include "ns3/wifi-module.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Simulation topology:

/*        Wired Network                      Wireless Network (802.11g)
 *    ===========================    ================================================
 *
 *         [n1: Source]
 *              |
 *              | (100 Mbps, 2ms delay)
 *              |
 *              v
 *    [n2: Bottleneck Router]
 *    [   AQM: SRED applied ]
 *              |
 *              | (5 Mbps, 5ms delay)
 *              |
 *              v
 *    [n3: WiFi Gateway] - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 *                                                |                |
 *                                                |   (24 Mbps)    |
 *                                                v                v
 *                                         [n4: WiFi Sink]  [n5: WiFi Sink]
 *
 */
// link1: n1-n2 (100Mbps, 2ms)
// link2: n2-n3 (5Mbps, 5ms) - bottleneck link
// WiFi: n3, n4, n5 (802.11g, 24Mbps)

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SredBonusSimulation");

std::ofstream g_queueFile;

void
SampleQueueSize(Ptr<QueueDisc> queueDisc)
{
    uint32_t pkts = queueDisc->GetNPackets();
    if (g_queueFile.is_open())
    {
        g_queueFile << Simulator::Now().GetSeconds() << " " << pkts << std::endl;
    }
    // Sample every 10ms
    Simulator::Schedule(MilliSeconds(10), &SampleQueueSize, queueDisc);
}

int
main(int argc, char* argv[])
{
    std::string queueType = "SRED"; // SRED, RED, or DropTail
    double simTime = 50.0;
    std::string outputFile = "scratch/queue_size.txt";

    CommandLine cmd;
    cmd.AddValue("queueType", "AQM Type (SRED, RED, DropTail)", queueType);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("outputFile", "Output file for queue size data", outputFile);
    cmd.Parse(argc, argv);

    // 1. Create Nodes
    // n1 (0), n2 (1), n3 (2), n4 (3), n5 (4)
    NodeContainer allNodes;
    allNodes.Create(5);

    Ptr<Node> n1 = allNodes.Get(0);
    Ptr<Node> n2 = allNodes.Get(1);
    Ptr<Node> n3 = allNodes.Get(2);
    Ptr<Node> n4 = allNodes.Get(3);
    Ptr<Node> n5 = allNodes.Get(4);

    NodeContainer wiredLink1(n1, n2);
    NodeContainer wiredLink2(n2, n3);
    NodeContainer wifiNodes(n3, n4, n5);

    // 2. Set up Point-to-Point Links (Wired)
    PointToPointHelper p2pFast;
    p2pFast.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pFast.SetChannelAttribute("Delay", StringValue("2ms"));

    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate", StringValue("5Mbps")); // bottleneck link n2->n3
    bottleneck.SetChannelAttribute("Delay", StringValue("5ms"));

    NetDeviceContainer link1Devs = p2pFast.Install(wiredLink1);
    NetDeviceContainer link2Devs = bottleneck.Install(wiredLink2);

    // 3. Set up WiFi (Wireless)
    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());

    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac"); // Ad-hoc mode for simplicity, connecting n3, n4, n5

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211g);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",
                                 StringValue("ErpOfdmRate24Mbps"),
                                 "ControlMode",
                                 StringValue("ErpOfdmRate24Mbps"));

    NetDeviceContainer wifiDevs = wifi.Install(phy, mac, wifiNodes);

    // Set Mobility for WiFi nodes (static layout)
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator>();
    positionAlloc->Add(Vector(0.0, 0.0, 0.0));  // n3
    positionAlloc->Add(Vector(10.0, 0.0, 0.0)); // n4
    positionAlloc->Add(Vector(0.0, 10.0, 0.0)); // n5
    mobility.SetPositionAllocator(positionAlloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(wifiNodes);

    // 4. Install Internet Stack
    InternetStackHelper stack;
    stack.Install(allNodes);

    // 5. Assign IP Addresses
    Ipv4AddressHelper ipv4;

    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer link1Interfaces = ipv4.Assign(link1Devs);

    ipv4.SetBase("10.1.2.0", "255.255.255.0");
    Ipv4InterfaceContainer link2Interfaces = ipv4.Assign(link2Devs);

    ipv4.SetBase("10.1.3.0", "255.255.255.0");
    Ipv4InterfaceContainer wifiInterfaces = ipv4.Assign(wifiDevs);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // 6. Setup AQM on bottleneck interface at n2 (interface going to n3)
    TrafficControlHelper tch;
    uint32_t queueSizePkts = 300; // 300 packets

    if (queueType == "SRED")
    {
        tch.SetRootQueueDisc("ns3::SredQueueDiscMod",
                             "MaxSize",
                             StringValue(std::to_string(queueSizePkts) + "p"),
                             "ZombieListSize",
                             UintegerValue(10));
        Config::SetDefault("ns3::SredQueueDiscMod::MaxTh", DoubleValue(300));
    }
    // else if (queueType == "RED")
    // {
    //     tch.SetRootQueueDisc("ns3::RedQueueDisc",
    //                          "MaxSize",
    //                          StringValue(std::to_string(queueSizePkts) + "p"),
    //                          "MinTh",
    //                          StringValue(std::to_string(queueSizePkts / 4) + "p"),
    //                          "MaxTh",
    //                          StringValue(std::to_string(queueSizePkts * 3 / 4) + "p"),
    //                          "LinkBandwidth",
    //                          StringValue("5Mbps"),
    //                          "LinkDelay",
    //                          StringValue("5ms"));
    // }
    // else // DropTail
    // {
    //     tch.SetRootQueueDisc("ns3::FifoQueueDisc",
    //                          "MaxSize",
    //                          StringValue(std::to_string(queueSizePkts) + "p"));
    // }

    // Install on n2's NetDevice connected to Link 2
    tch.Uninstall(link2Devs.Get(0));
    QueueDiscContainer qdContainer = tch.Install(link2Devs.Get(0));

    // 7. Schedule Queue Monitoring
    g_queueFile.open(outputFile);
    if (!g_queueFile.is_open())
    {
        std::cerr << "Cannot open " << outputFile << " for writing queue size!" << std::endl;
        return 1;
    }
    Simulator::Schedule(MilliSeconds(10), &SampleQueueSize, qdContainer.Get(0));

    // 8. Applications - Cross Network Traffic from n1 to n4 and n5
    uint16_t port = 5000;

    // TCP sink on n4
    Address sinkAddressN4(InetSocketAddress(wifiInterfaces.GetAddress(1), port));
    PacketSinkHelper packetSinkN4("ns3::TcpSocketFactory",
                                  InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkAppN4 = packetSinkN4.Install(n4);
    sinkAppN4.Start(Seconds(0.0));
    sinkAppN4.Stop(Seconds(simTime + 1));

    // TCP source on n1 to n4
    BulkSendHelper sourceN4("ns3::TcpSocketFactory", sinkAddressN4);
    sourceN4.SetAttribute("MaxBytes", UintegerValue(0)); // send indefinitely
    ApplicationContainer sourceAppN4 = sourceN4.Install(n1);
    sourceAppN4.Start(Seconds(1.0));
    sourceAppN4.Stop(Seconds(simTime));

    port++;

    // TCP sink on n5
    Address sinkAddressN5(InetSocketAddress(wifiInterfaces.GetAddress(2), port));
    PacketSinkHelper packetSinkN5("ns3::TcpSocketFactory",
                                  InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkAppN5 = packetSinkN5.Install(n5);
    sinkAppN5.Start(Seconds(0.0));
    sinkAppN5.Stop(Seconds(simTime + 1));

    // TCP source on n1 to n5
    BulkSendHelper sourceN5("ns3::TcpSocketFactory", sinkAddressN5);
    sourceN5.SetAttribute("MaxBytes", UintegerValue(0));
    ApplicationContainer sourceAppN5 = sourceN5.Install(n1);
    sourceAppN5.Start(Seconds(1.5)); // Start slightly offset to represent varied flow initiation
    sourceAppN5.Stop(Seconds(simTime));

    port++;

    // UDP source on n1 to n4 (represents some background traffic)
    uint32_t payloadSize = 1000;
    Address udpSinkAddressN4(InetSocketAddress(wifiInterfaces.GetAddress(1), port));
    PacketSinkHelper udpSinkN4("ns3::UdpSocketFactory",
                               InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer udpSinkApp = udpSinkN4.Install(n4);
    udpSinkApp.Start(Seconds(0.0));
    udpSinkApp.Stop(Seconds(simTime));

    OnOffHelper onoffUDP("ns3::UdpSocketFactory", udpSinkAddressN4);
    onoffUDP.SetConstantRate(DataRate("1Mbps"), payloadSize);
    ApplicationContainer udpSourceApp = onoffUDP.Install(n1);
    udpSourceApp.Start(Seconds(2.0));
    udpSourceApp.Stop(Seconds(simTime));

    std::cout << "Starting simulation with " << queueType << " AQM..." << std::endl;
    std::cout << "Topology: n1 (wired) -> n2 (bottleneck) -> n3 (AP) -> n4/n5 (wifi)" << std::endl;

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    g_queueFile.close();

    // Throughput reporting
    uint64_t totalN4TCP = DynamicCast<PacketSink>(sinkAppN4.Get(0))->GetTotalRx();
    uint64_t totalN5TCP = DynamicCast<PacketSink>(sinkAppN5.Get(0))->GetTotalRx();
    uint64_t totalN4UDP = DynamicCast<PacketSink>(udpSinkApp.Get(0))->GetTotalRx();

    std::cout << "\n--- Results ---" << std::endl;
    std::cout << "Flow 1 (n1->n4 TCP) Rx Bytes : " << totalN4TCP << std::endl;
    std::cout << "Flow 2 (n1->n5 TCP) Rx Bytes : " << totalN5TCP << std::endl;
    std::cout << "Flow 3 (n1->n4 UDP) Rx Bytes : " << totalN4UDP << std::endl;

    Simulator::Destroy();

    return 0;
}

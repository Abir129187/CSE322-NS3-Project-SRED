/*
 * Delay-Aware SRED Simulation — Wired Bottleneck Topology
 *
 * Topology: Star-Bottleneck
 *
 *
 *          Access Links                         Bottleneck Link
 * =============================    ========================================

 *  [Source 1] -----------\
 *                         \
 *  [Source 2] -------------\
 *                           \
 *     ...                   ------> [Router] --------------> [Sink]
 *                           /      [AQM: SRED]
 *  [Source N-1] -----------/        (1.5 Mbps, 20ms)
 *                         /
 *  [Source N] -----------/

 *   (100 Mbps, varying delay)
 *
 *  Access Links: 100 Mbps, delay based on rangeScale (1-5 x base delay)
 * Bottleneck Link: 1.5 Mbps, 20 ms delay
 * Traffic: CBR UDP from each source to sink, with packetsPerSec rate
 *
 *
 * Parameters varied:
 *   --nNodes      : Total nodes (20, 40, 60, 80, 100)           default=20
 *   --nFlows      : Number of UDP flows (10, 20, 30, 40, 50)    default=10
 *   --packetsPerSec: Packets per second per flow (100..500)      default=100
 *   --rangeScale  : multiplier for link length (1, 2, 3, 4, 5)  default=1
 *   --simTime     : Simulation duration (s)                     default=30
 *
 * Metrics collected:
 *   1. Network throughput (kbps)
 *   2. End-to-end average delay (ms)
 *   3. Packet delivery ratio
 *   4. Packet drop ratio
 *   5. Energy consumption (Set to 0.0 for wired)
 *
 * Output: CSV line to stdout.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-layer.h"
#include "ns3/traffic-control-module.h"

#include <iomanip>
#include <iostream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SredWiredSim");

int
main(int argc, char* argv[])
{
    // ========== Parameters ==========
    uint32_t nNodes = 20;
    uint32_t nFlows = 10;
    uint32_t packetsPerSec = 100;
    uint32_t rangeScale = 1; // 1 to 5, as Tx_range multiplier
    double simTime = 30.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("nNodes", "Total number of nodes", nNodes);
    cmd.AddValue("nFlows", "Number of UDP flows", nFlows);
    cmd.AddValue("packetsPerSec", "Packets per second per flow", packetsPerSec);
    cmd.AddValue("rangeScale", "Coverage area scale (1-5 x Tx_range)", rangeScale);
    cmd.AddValue("simTime", "Simulation time (s)", simTime);
    cmd.Parse(argc, argv);

    if (nNodes < 3)
    {
        NS_ABORT_MSG("nNodes must be at least 3 (1 source, 1 router, 1 sink)");
    }

    // ========== Network Topology ==========
    NodeContainer sources;
    sources.Create(nNodes - 2);

    Ptr<Node> router = CreateObject<Node>();
    Ptr<Node> sink = CreateObject<Node>();

    // Link Properties
    // Base Tx_range for wired simulation (interpreted as base link length)
    double baseDistance = 100.0;                              // meters
    double speedOfLight = 3e8;                                // m/s
    double baseDelayUs = (baseDistance / speedOfLight) * 1e6; // ~0.33us
    Time sourceLinkDelay = MicroSeconds(baseDelayUs * rangeScale);

    PointToPointHelper accessLink;
    accessLink.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    accessLink.SetChannelAttribute("Delay", TimeValue(sourceLinkDelay));

    PointToPointHelper bottleneckLink;
    bottleneckLink.SetDeviceAttribute("DataRate", StringValue("1.5Mbps"));
    bottleneckLink.SetChannelAttribute("Delay", TimeValue(MilliSeconds(20)));

    // Connect Sources to Router
    NetDeviceContainer routerDevices; // devices on router facing sources
    NetDeviceContainer sourceDevices;
    for (uint32_t i = 0; i < sources.GetN(); ++i)
    {
        NetDeviceContainer link = accessLink.Install(sources.Get(i), router);
        sourceDevices.Add(link.Get(0));
        routerDevices.Add(link.Get(1));
    }

    // Connect Router to Sink (Bottleneck)
    NetDeviceContainer bottleneck = bottleneckLink.Install(router, sink);
    Ptr<NetDevice> routerBottleneckDev = bottleneck.Get(0);
    Ptr<NetDevice> sinkDev = bottleneck.Get(1);

    // ========== Internet Stack ==========
    InternetStackHelper stack;
    stack.InstallAll();

    Ipv4AddressHelper address;
    // Assign addresses for access links: 10.1.x.0
    for (uint32_t i = 0; i < sources.GetN(); ++i)
    {
        std::ostringstream subnet;
        subnet << "10.1." << (i + 1) << ".0";
        address.SetBase(subnet.str().c_str(), "255.255.255.0");
        address.Assign(NetDeviceContainer(sourceDevices.Get(i), routerDevices.Get(i)));
    }

    // Assign address for bottleneck link: 10.2.1.0
    address.SetBase("10.2.1.0", "255.255.255.0");
    Ipv4InterfaceContainer bottleneckInterfaces = address.Assign(bottleneck);
    Ipv4Address sinkAddress = bottleneckInterfaces.GetAddress(1);

    // ========== Traffic Control with SredQueueDiscMod ==========
    TrafficControlHelper tc;
    tc.SetRootQueueDisc("ns3::SredQueueDiscMod",
                        "MaxSize",
                        StringValue("50p"),
                        "MaxTh",
                        DoubleValue(50),
                        "PMax",
                        DoubleValue(0.15),
                        "DelayGamma",
                        DoubleValue(1.0),
                        "DelayExponentK",
                        DoubleValue(2.0),
                        "DelayReference",
                        TimeValue(MilliSeconds(20)));

    // Install SRED on the router's bottleneck interface
    // Note: We install it ONCE on the specific device container.
    // We first delete any default queue disc that might have been auto-installed.
    Ptr<TrafficControlLayer> tcLayer = router->GetObject<TrafficControlLayer>();
    tcLayer->DeleteRootQueueDiscOnDevice(bottleneck.Get(0));
    NetDeviceContainer bottleneckRouterDev;
    bottleneckRouterDev.Add(bottleneck.Get(0));
    tc.Install(bottleneckRouterDev);

    // Static Routing
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ========== Applications ==========
    uint16_t port = 9000;
    // Sink on node 'sink'
    for (uint32_t f = 0; f < nFlows; f++)
    {
        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port + f));
        ApplicationContainer sinkApp = sinkHelper.Install(sink);
        sinkApp.Start(Seconds(1.0));
        sinkApp.Stop(Seconds(simTime));
    }

    // UDP Flows
    for (uint32_t f = 0; f < nFlows; f++)
    {
        uint32_t srcIdx = f % sources.GetN();
        OnOffHelper onoff("ns3::UdpSocketFactory", InetSocketAddress(sinkAddress, port + f));

        uint32_t pktSize = 1000; // Large packets for wired
        double dataRate = pktSize * 8.0 * packetsPerSec;

        onoff.SetConstantRate(DataRate(static_cast<uint64_t>(dataRate)), pktSize);
        onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));

        ApplicationContainer senderApp = onoff.Install(sources.Get(srcIdx));
        senderApp.Start(Seconds(2.0));
        senderApp.Stop(Seconds(simTime - 1));
    }

    // ========== Flow Monitor ==========
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> flowMonitor = fmHelper.InstallAll();

    // ========== Run ==========
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // ========== Metrics ==========
    flowMonitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());

    double totalThroughput = 0.0;
    double totalDelay = 0.0;
    uint64_t totalRxPackets = 0;
    uint64_t totalTxPackets = 0;
    uint64_t totalLostPackets = 0;

    std::map<FlowId, FlowMonitor::FlowStats> stats = flowMonitor->GetFlowStats();
    for (auto& kv : stats)
    {
        FlowMonitor::FlowStats& fs = kv.second;
        totalTxPackets += fs.txPackets;
        totalRxPackets += fs.rxPackets;
        totalLostPackets += fs.lostPackets;

        if (fs.rxPackets > 0)
        {
            double duration = (fs.timeLastRxPacket - fs.timeFirstTxPacket).GetSeconds();
            if (duration > 0)
            {
                totalThroughput += (fs.rxBytes * 8.0) / duration / 1000.0; // kbps
            }
            totalDelay += fs.delaySum.GetMilliSeconds();
        }
    }

    double avgDelay = (totalRxPackets > 0) ? (totalDelay / totalRxPackets) : 0.0;
    double pdr = (totalTxPackets > 0) ? ((double)totalRxPackets / totalTxPackets) : 0.0;
    double dropRatio = (totalTxPackets > 0) ? ((double)totalLostPackets / totalTxPackets) : 0.0;

    // Output CSV: nNodes,nFlows,PPS,rangeScale,throughput,delay,PDR,dropRatio,energy
    std::cout << nNodes << "," << nFlows << "," << packetsPerSec << "," << rangeScale << ","
              << std::fixed << std::setprecision(4) << totalThroughput << "," << avgDelay << ","
              << pdr << "," << dropRatio << "," << 0.0 // Energy is 0 for wired
              << std::endl;

    Simulator::Destroy();
    return 0;
}

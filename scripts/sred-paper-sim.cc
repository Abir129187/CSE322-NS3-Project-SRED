#include "ns3/applications-module.h"
#include "ns3/config-store-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/sred-queue-disc-mod.h"
#include "ns3/traffic-control-module.h"

#include <fstream>
#include <iostream>

//
// Network Topology
//
//   Left Hosts (nFlows)                           Right Hosts (nFlows)
//   (BulkSendApps)                                (PacketSinks)
//
//      11.0.0.0/30                                      12.0.0.0/30
//   L0 -------------                               ------------- R0
//  (11.x.x.x/30)    \                             /    (12.y.y.y/30)
//   L1 ------------- \       10.1.1.0/30         / ------------- R1
//       ...           - r1 ================= r2 -        ...
//   L(n-1) --------- /      Bottleneck Link      \ ------------- R(n-1)
//                   /           45 Mbps
//                  /              1 ms
//                 -           (SRED / RED)          -
//
//   Access links:                                 Access links:
//     DataRate: 45 Mbps                             DataRate: 45 Mbps
//     Delay (sym): 10 ms                            Delay (sym): 10 ms
//     Delay (asym): 1-20 ms                         Delay (asym): 1-20 ms

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SredPaperSimulation");

std::ofstream g_bufferFile;

void
SampleBufferState(Ptr<QueueDisc> queueDisc)
{
    uint32_t bytes = queueDisc->GetNBytes();
    if (g_bufferFile.is_open())
    {
        g_bufferFile << Simulator::Now().GetSeconds() << " " << bytes << std::endl;
    }
    Simulator::Schedule(MilliSeconds(10), &SampleBufferState, queueDisc);
}

int
main(int argc, char* argv[])
{
    uint32_t nFlows = 10;
    std::string topologyType = "symmetrical"; // 'symmetrical' or 'asymmetrical'
    std::string queueDiscType = "SRED";       // 'SRED' or 'RED'
    double simTime = 50.0;
    std::string queueDiscName = "ns3::SredQueueDiscMod";
    std::string prefix = "scratch/results/paper/";

    CommandLine cmd;
    cmd.AddValue("nFlows", "Number of host pairs (flows)", nFlows);
    cmd.AddValue("topologyType", "Topology type: 'symmetrical' or 'asymmetrical'", topologyType);
    cmd.AddValue("queueDiscType", "Queue Discipline type: 'SRED' or 'RED'", queueDiscType);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("prefix", "Prefix for output files", prefix);
    cmd.Parse(argc, argv);

    // File naming mappings
    std::string bufferFn =
        prefix + "buffer_" + topologyType + "_" + std::to_string(nFlows) + ".txt";
    std::string tpFn =
        prefix + "throughput_" + topologyType + "_" + std::to_string(nFlows) + ".txt";

    g_bufferFile.open(bufferFn);
    if (!g_bufferFile.is_open())
    {
        std::cerr << "Could not open " << bufferFn
                  << " for writing. Create directory 'scratch/results/paper' first!" << std::endl;
        return 1;
    }

    // MaxTh (B) set to 500,000 bytes as requested to match plot limit
    Config::SetDefault("ns3::SredQueueDiscMod::MaxTh", DoubleValue(500000));
    // Smoother EWMA for hit probability estimate
    Config::SetDefault("ns3::SredQueueDiscMod::HitProbEwma", DoubleValue(0.0002));

    // Use a more 'classic' TCP (NewReno) to match the paper's era
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpNewReno"));
    // Disable Delayed ACKs to make the flow more predictable
    Config::SetDefault("ns3::TcpSocket::DelAckCount", UintegerValue(1));
    // MTU = 576 bytes; MSS = 536 bytes + 40 bytes IP/TCP header
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(536));
    // Provide huge buffers so TCP purely bottlenecked by link
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(10000000));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(10000000));
    Config::SetDefault("ns3::TcpSocket::InitialCwnd", UintegerValue(1));

    if (queueDiscType == "RED")
    {
        queueDiscName = "ns3::RedQueueDisc";
    }

    NodeContainer routers;
    routers.Create(2);
    Ptr<Node> r1 = routers.Get(0);
    Ptr<Node> r2 = routers.Get(1);

    NodeContainer leftHosts, rightHosts;
    leftHosts.Create(nFlows);
    rightHosts.Create(nFlows);

    InternetStackHelper stack;
    stack.Install(routers);
    stack.Install(leftHosts);
    stack.Install(rightHosts);

    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate", StringValue("45Mbps"));
    bottleneck.SetChannelAttribute("Delay", StringValue("1ms"));

    NetDeviceContainer routerDevices = bottleneck.Install(routers);

    TrafficControlHelper tch;
    uint32_t B = 500000; // Capacity: 500,000 bytes

    if (queueDiscType == "SRED")
    {
        tch.SetRootQueueDisc(queueDiscName, "MaxSize", StringValue(std::to_string(B) + "B"));
    }
    else if (queueDiscType == "RED")
    {
        tch.SetRootQueueDisc(queueDiscName,
                             "QW",
                             DoubleValue(0.002), // 1/500
                             "LInterm",
                             DoubleValue(1.0 / 0.075),
                             "MinTh",
                             StringValue(std::to_string(B / 6) + "B"),
                             "MaxTh",
                             StringValue(std::to_string(B / 3) + "B"),
                             "MaxSize",
                             StringValue(std::to_string(B) + "B"));
    }

    QueueDiscContainer queueDiscs = tch.Install(routerDevices);

    // Schedule Buffer Sampling every 10ms
    Simulator::Schedule(MilliSeconds(10), &SampleBufferState, queueDiscs.Get(0));

    std::vector<NetDeviceContainer> leftAccessDevices(nFlows);
    std::vector<NetDeviceContainer> rightAccessDevices(nFlows);

    for (uint32_t i = 0; i < nFlows; ++i)
    {
        PointToPointHelper accessLink;
        accessLink.SetDeviceAttribute("DataRate", StringValue("45Mbps"));

        double delayMs = 10.0;
        if (topologyType == "asymmetrical")
        {
            delayMs = 1.0;
            if (nFlows > 1)
            {
                delayMs = 1.0 + 19.0 * ((double)i / (nFlows - 1));
            }
        }
        accessLink.SetChannelAttribute("Delay", StringValue(std::to_string(delayMs) + "ms"));

        leftAccessDevices[i] = accessLink.Install(leftHosts.Get(i), r1);
        rightAccessDevices[i] = accessLink.Install(rightHosts.Get(i), r2);
    }

    Ipv4AddressHelper routerIpv4;
    routerIpv4.SetBase("10.1.1.0", "255.255.255.252");
    routerIpv4.Assign(routerDevices);

    Ipv4AddressHelper leftIpv4;
    leftIpv4.SetBase("11.0.0.0", "255.255.255.252");
    Ipv4AddressHelper rightIpv4;
    rightIpv4.SetBase("12.0.0.0", "255.255.255.252");

    std::vector<Ipv4InterfaceContainer> rightInterfaces(nFlows);

    for (uint32_t i = 0; i < nFlows; ++i)
    {
        leftIpv4.Assign(leftAccessDevices[i]);
        leftIpv4.NewNetwork();
        rightInterfaces[i] = rightIpv4.Assign(rightAccessDevices[i]);
        rightIpv4.NewNetwork();
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Applications
    uint16_t port = 50000;
    ApplicationContainer sinkApps;

    for (uint32_t i = 0; i < nFlows; ++i)
    {
        Address sinkAddress(InetSocketAddress(rightInterfaces[i].GetAddress(0), port));
        PacketSinkHelper sink("ns3::TcpSocketFactory",
                              InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer app = sink.Install(rightHosts.Get(i));
        app.Start(Seconds(0.0));
        app.Stop(Seconds(simTime));
        sinkApps.Add(app);

        BulkSendHelper source("ns3::TcpSocketFactory", sinkAddress);
        source.SetAttribute("MaxBytes", UintegerValue(0)); // Infinite file
        ApplicationContainer sourceApps = source.Install(leftHosts.Get(i));
        sourceApps.Start(Seconds(0.0));
        sourceApps.Stop(Seconds(simTime));
        port++;
    }

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // Sink Data Tracking for File Generation
    std::ofstream outTp(tpFn);
    if (outTp.is_open())
    {
        double totalThroughput = 0.0;
        std::vector<double> throughputs(nFlows);
        for (uint32_t i = 0; i < nFlows; ++i)
        {
            Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApps.Get(i));
            uint64_t bytes = sink->GetTotalRx();
            double tpMbps = (bytes * 8.0) / simTime / 1000000.0;
            throughputs[i] = tpMbps;
            totalThroughput += tpMbps;
        }

        outTp << "FlowID Throughput_Mbps Normalized\n";
        for (uint32_t i = 0; i < nFlows; ++i)
        {
            double normalized = (totalThroughput > 0) ? (throughputs[i] / totalThroughput) : 0;
            outTp << i + 1 << " " << throughputs[i] << " " << normalized << "\n";
        }
        outTp.close();
    }

    if (g_bufferFile.is_open())
    {
        g_bufferFile.close();
    }

    Simulator::Destroy();
    return 0;
}

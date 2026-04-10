/*
 * Delay-Aware SRED Simulation over IEEE 802.15.4 (Mobile)
 *
 * Simulates a mobile wireless sensor network using LR-WPAN (802.15.4)
 * with SixLoWPAN/IPv6 stack and the Delay-Aware SRED queue disc.
 *
 *
 * Network topology:
 *
 * Random Waypoint Mobility Area (Default: 100m x 100m)
 *  ================================================================
 *
 *        [Node 1] ~ ~ ~ ~ ~ \
 *      (Sender +  SRED)      \
 *                             \
 *        [Node 2] ~ ~ ~ ~ ~ ~ ~\
 *       (Sender +  SRED)        \  (IPv6 over 802.15.4)
 *                                ~ ~ ~ ~ >  [Node 0]
 *           ...                             (UDP Sink)
 *                                           [AQM: SRED]
 *        [Node N] ~ ~ ~ ~ ~ ~ ~/
 *         (Sender +  SRED)    /
 *                            /
 *        [Node X] ~ ~ ~ ~ ~ /
 *         (Sender +  SRED)
 *
 *  (Note: All nodes, including Node 0, are continuously moving)
 *
 *
 * Parameters varied (one at a time, others held at default):
 *   --nNodes      : Number of nodes (20, 40, 60, 80, 100)         default=20
 *   --nFlows      : Number of flows (10, 20, 30, 40, 50)          default=10
 *   --packetsPerSec: Packets per second per flow (100..500)        default=100
 *   --speed       : Node speed in m/s (5, 10, 15, 20, 25)         default=5
 *   --areaSize    : Coverage area side length in m                 default=100
 *   --simTime     : Simulation duration in seconds                 default=30
 *
 * Metrics collected:
 *   1. Network throughput (kbps)
 *   2. End-to-end average delay (ms)
 *   3. Packet delivery ratio
 *   4. Packet drop ratio
 *   5. Total energy consumption (J)
 *
 * Output: single-line CSV to stdout:
 *   nNodes,nFlows,PPS,speed,throughput,delay,PDR,dropRatio,energy
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/energy-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/lr-wpan-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/propagation-module.h"
#include "ns3/sixlowpan-module.h"
#include "ns3/spectrum-module.h"
#include "ns3/traffic-control-module.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace ns3;
using namespace ns3::lrwpan;

NS_LOG_COMPONENT_DEFINE("SredLrWpanSim");

// ---------- Global energy tracking ----------
// Since ns-3 has no built-in LrWpanRadioEnergyModel, we track energy
// by counting PHY state transitions and accumulating time-in-state.
// Typical 802.15.4 power draw (CC2420-like):
//   TX:   31.2  mW  (17.4  mA @ 1.8  V, or ~36.5 mW at 2.1V typical)
//   RX:   35.46 mW  (19.7  mA)
//   IDLE:  0.712 mW (0.396 mA)   (TRX_OFF)
//   SLEEP: 0.0006 mW

static double g_totalEnergyJ = 0.0;
static const double TX_POWER_W = 0.0312;     // 31.2 mW
static const double RX_POWER_W = 0.03546;    // 35.46 mW
static const double IDLE_POWER_W = 0.000712; // 0.712 mW

struct PhyStateInfo
{
    PhyEnumeration state;
    Time lastChange;
};

static std::map<uint32_t, PhyStateInfo> g_phyStates;

static void
PhyStateChange(std::string context, Time /*now*/, PhyEnumeration oldState, PhyEnumeration newState)
{
    // Parse node id from context path
    uint32_t nodeId = 0;
    std::string::size_type pos = context.find("/NodeList/");
    if (pos != std::string::npos)
    {
        std::string sub = context.substr(pos + 10);
        nodeId = std::stoi(sub.substr(0, sub.find('/')));
    }

    Time now = Simulator::Now();
    auto it = g_phyStates.find(nodeId);
    if (it != g_phyStates.end())
    {
        double dt = (now - it->second.lastChange).GetSeconds();
        PhyEnumeration prev = it->second.state;
        double power = IDLE_POWER_W;
        if (prev == IEEE_802_15_4_PHY_TX_ON || prev == IEEE_802_15_4_PHY_BUSY_TX)
        {
            power = TX_POWER_W;
        }
        else if (prev == IEEE_802_15_4_PHY_RX_ON || prev == IEEE_802_15_4_PHY_BUSY_RX)
        {
            power = RX_POWER_W;
        }
        g_totalEnergyJ += power * dt;
    }
    g_phyStates[nodeId] = {newState, now};
}

// Finalize energy accounting at end of simulation
static void
FinalizeEnergy()
{
    Time now = Simulator::Now();
    for (auto& kv : g_phyStates)
    {
        double dt = (now - kv.second.lastChange).GetSeconds();
        PhyEnumeration prev = kv.second.state;
        double power = IDLE_POWER_W;
        if (prev == IEEE_802_15_4_PHY_TX_ON || prev == IEEE_802_15_4_PHY_BUSY_TX)
        {
            power = TX_POWER_W;
        }
        else if (prev == IEEE_802_15_4_PHY_RX_ON || prev == IEEE_802_15_4_PHY_BUSY_RX)
        {
            power = RX_POWER_W;
        }
        g_totalEnergyJ += power * dt;
    }
}

int
main(int argc, char* argv[])
{
    // ========== Command-line parameters ==========
    uint32_t nNodes = 20;
    uint32_t nFlows = 10;
    uint32_t packetsPerSec = 100;
    double speed = 5.0;    // m/s
    double areaSize = 100; // metres (side of square)
    double simTime = 30.0; // seconds

    CommandLine cmd(__FILE__);
    cmd.AddValue("nNodes", "Number of nodes", nNodes);
    cmd.AddValue("nFlows", "Number of UDP flows", nFlows);
    cmd.AddValue("packetsPerSec", "Packets per second per flow", packetsPerSec);
    cmd.AddValue("speed", "Node speed (m/s) for RandomWaypoint mobility", speed);
    cmd.AddValue("areaSize", "Side length of square coverage area (m)", areaSize);
    cmd.AddValue("simTime", "Simulation time (s)", simTime);
    cmd.Parse(argc, argv);

    // ========== Create nodes ==========
    NodeContainer nodes;
    nodes.Create(nNodes);

    // ========== Mobility: RandomWaypointMobilityModel ==========
    MobilityHelper mobility;

    // Position allocator for the initial positions — uniform random in [0, areaSize]^2
    Ptr<RandomRectanglePositionAllocator> posAlloc =
        CreateObject<RandomRectanglePositionAllocator>();
    posAlloc->SetAttribute(
        "X",
        StringValue("ns3::UniformRandomVariable[Min=0|Max=" + std::to_string(areaSize) + "]"));
    posAlloc->SetAttribute(
        "Y",
        StringValue("ns3::UniformRandomVariable[Min=0|Max=" + std::to_string(areaSize) + "]"));

    // Waypoint position allocator for the RandomWaypointMobilityModel
    Ptr<RandomRectanglePositionAllocator> waypointAlloc =
        CreateObject<RandomRectanglePositionAllocator>();
    waypointAlloc->SetAttribute(
        "X",
        StringValue("ns3::UniformRandomVariable[Min=0|Max=" + std::to_string(areaSize) + "]"));
    waypointAlloc->SetAttribute(
        "Y",
        StringValue("ns3::UniformRandomVariable[Min=0|Max=" + std::to_string(areaSize) + "]"));

    mobility.SetPositionAllocator(posAlloc);
    mobility.SetMobilityModel(
        "ns3::RandomWaypointMobilityModel",
        "Speed",
        StringValue("ns3::ConstantRandomVariable[Constant=" + std::to_string(speed) + "]"),
        "Pause",
        StringValue("ns3::ConstantRandomVariable[Constant=0.5]"),
        "PositionAllocator",
        PointerValue(waypointAlloc));
    mobility.Install(nodes);

    // ========== LR-WPAN (802.15.4) ==========
    LrWpanHelper lrWpanHelper;
    lrWpanHelper.SetPropagationDelayModel("ns3::ConstantSpeedPropagationDelayModel");
    lrWpanHelper.AddPropagationLossModel("ns3::LogDistancePropagationLossModel");

    NetDeviceContainer lrwpanDevices = lrWpanHelper.Install(nodes);

    // Associate all devices to PAN ID 1 (assigns short addresses automatically)
    lrWpanHelper.CreateAssociatedPan(lrwpanDevices, 1);

    // ========== SixLoWPAN + IPv6 ==========
    InternetStackHelper internetv6;
    internetv6.Install(nodes);

    SixLowPanHelper sixlowpan;
    NetDeviceContainer sixDevices = sixlowpan.Install(lrwpanDevices);

    Ipv6AddressHelper ipv6;
    ipv6.SetBase(Ipv6Address("2001:1::"), Ipv6Prefix(64));
    Ipv6InterfaceContainer deviceInterfaces = ipv6.Assign(sixDevices);

    // ========== Install traffic control with SredQueueDiscMod ==========
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
    tc.Install(sixDevices);

    // ========== PHY state tracing for energy ==========
    for (uint32_t i = 0; i < nodes.GetN(); i++)
    {
        std::string path = "/NodeList/" + std::to_string(i) +
                           "/DeviceList/*/$ns3::lrwpan::LrWpanNetDevice/Phy/TrxState";
        Config::Connect(path, MakeCallback(&PhyStateChange));
        // Initialize state tracking
        g_phyStates[i] = {IEEE_802_15_4_PHY_TRX_OFF, Seconds(0)};
    }

    // ========== Applications: UDP traffic ==========
    // Node 0 acts as sink for all flows.
    // Nodes 1..(nFlows) each send a UDP stream to node 0.
    uint16_t basePort = 9000;

    // Install packet sinks on node 0
    for (uint32_t f = 0; f < nFlows; f++)
    {
        uint16_t port = basePort + f;
        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    Inet6SocketAddress(Ipv6Address::GetAny(), port));
        ApplicationContainer sinkApp = sinkHelper.Install(nodes.Get(0));
        sinkApp.Start(Seconds(1.0));
        sinkApp.Stop(Seconds(simTime));
    }

    // Install UDP senders on nodes 1..nFlows
    for (uint32_t f = 0; f < nFlows; f++)
    {
        uint32_t srcNode = (f % (nNodes - 1)) + 1;
        uint16_t port = basePort + f;

        // Get destination (node 0) IPv6 address (index 1 = global address)
        Ipv6Address dstAddr = deviceInterfaces.GetAddress(0, 1);

        OnOffHelper onoff("ns3::UdpSocketFactory", Inet6SocketAddress(dstAddr, port));

        // 802.15.4 max payload is small (~100 bytes with 6LoWPAN overhead)
        uint32_t pktSize = 50;                           // bytes — fits 802.15.4 frame
        double dataRate = pktSize * 8.0 * packetsPerSec; // bits/sec

        onoff.SetConstantRate(DataRate(static_cast<uint64_t>(dataRate)), pktSize);
        onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));

        ApplicationContainer senderApp = onoff.Install(nodes.Get(srcNode));
        senderApp.Start(Seconds(2.0));
        senderApp.Stop(Seconds(simTime - 1));
    }

    // ========== Flow Monitor ==========
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> flowMonitor = fmHelper.InstallAll();

    // ========== Run ==========
    Simulator::Stop(Seconds(simTime));
    Simulator::Schedule(Seconds(simTime - 0.01), &FinalizeEnergy);
    Simulator::Run();

    // ========== Collect metrics ==========
    flowMonitor->CheckForLostPackets();
    Ptr<Ipv6FlowClassifier> classifier = DynamicCast<Ipv6FlowClassifier>(fmHelper.GetClassifier6());

    double totalThroughput = 0.0;
    double totalDelay = 0.0;
    uint64_t totalRxPackets = 0;
    uint64_t totalTxPackets = 0;
    uint64_t totalLostPackets = 0;
    uint32_t flowCount = 0;

    std::map<FlowId, FlowMonitor::FlowStats> stats = flowMonitor->GetFlowStats();
    for (auto& kv : stats)
    {
        FlowMonitor::FlowStats& fs = kv.second;
        totalTxPackets += fs.txPackets;
        totalRxPackets += fs.rxPackets;
        totalLostPackets += fs.lostPackets;

        if (fs.rxPackets > 0)
        {
            double flowDuration = (fs.timeLastRxPacket - fs.timeFirstTxPacket).GetSeconds();
            if (flowDuration > 0)
            {
                totalThroughput += (fs.rxBytes * 8.0) / flowDuration / 1000.0; // kbps
            }
            totalDelay += fs.delaySum.GetMilliSeconds();
            flowCount++;
        }
    }

    double avgDelay = (totalRxPackets > 0) ? (totalDelay / totalRxPackets) : 0.0;
    double pdr = (totalTxPackets > 0) ? ((double)totalRxPackets / totalTxPackets) : 0.0;
    double dropRatio = (totalTxPackets > 0) ? ((double)totalLostPackets / totalTxPackets) : 0.0;

    // ========== Output CSV line ==========
    std::cout << nNodes << "," << nFlows << "," << packetsPerSec << "," << speed << ","
              << std::fixed << std::setprecision(4) << totalThroughput << "," << avgDelay << ","
              << pdr << "," << dropRatio << "," << g_totalEnergyJ << std::endl;

    Simulator::Destroy();
    return 0;
}

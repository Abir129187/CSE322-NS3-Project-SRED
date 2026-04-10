/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

// Default Network Topology
//
//       10.1.1.0
// n0 -------------- n1
//    point-to-point
//

using namespace ns3;

/*Define a Log component with a specific name
This macro should be used at the top of every file in which you want to use
the NS_LOG macro.This macro defines a new log component with the given name 
which can be later enabled or disabled using the ns3::LogComponentEnable and
ns3::LogComponentDisable functions or with NS_LOG environment variable.*/

NS_LOG_COMPONENT_DEFINE("FirstScriptExample");

int
main(int argc, char* argv[])
{
    CommandLine cmd(__FILE__);

    //Hook your own values in the parser
    uint32_t npackets = 1;
    cmd.AddValue("npackets", "Number of packets to echo", npackets);

    cmd.Parse(argc, argv);

    //sets the time to resolution to nanoseconds
    Time::SetResolution(Time::NS);

    //enable two logging components that we are using
    //the echo client and echo server applications
    LogComponentEnable("UdpEchoClientApplication", LOG_LEVEL_INFO);
    LogComponentEnable("UdpEchoServerApplication", LOG_LEVEL_INFO);

    // Add logging to code
    // NS_LOG_INFO("Creating topology");

    //create the ns-3 node objects that will represnt the computers in the simulation
    //the container calls down in to the ns-3 system proper to create
    //two Node objects and stores pointers to them  
    NodeContainer nodes;
    nodes.Create(2);

    // pointtopoint helper to configure and connect
    //ns-3 pointTopointNetDevice and pointTopointChannel 
    PointToPointHelper pointToPoint;

    //attribut
    pointToPoint.SetDeviceAttribute("DataRate", StringValue("5Mbps"));
    pointToPoint.SetChannelAttribute("Delay", StringValue("2ms"));

    NetDeviceContainer devices;
    devices = pointToPoint.Install(nodes);

    InternetStackHelper stack;
    stack.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");

    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    UdpEchoServerHelper echoServer(9);

    ApplicationContainer serverApps = echoServer.Install(nodes.Get(1));
    serverApps.Start(Seconds(1));
    serverApps.Stop(Seconds(10));

    UdpEchoClientHelper echoClient(interfaces.GetAddress(1), 9);
    echoClient.SetAttribute("MaxPackets", UintegerValue(1));
    echoClient.SetAttribute("Interval", TimeValue(Seconds(1)));
    echoClient.SetAttribute("PacketSize", UintegerValue(1024));

    ApplicationContainer clientApps = echoClient.Install(nodes.Get(0));
    clientApps.Start(Seconds(2));
    clientApps.Stop(Seconds(10));

    Simulator::Run();
    Simulator::Destroy();
    return 0;
}

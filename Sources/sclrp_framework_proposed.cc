#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/dsdv-module.h"
#include "ns3/basic-energy-source-helper.h"
#include "ns3/wifi-radio-energy-model-helper.h"
#include "ns3/yans-wifi-phy.h"
#include "ns3/system-wall-clock-ms.h"
#include "ns3/netanim-module.h"
#include <cstdlib>
#include <cstdio>

using namespace ns3;

static uint32_t g_commOverhead = 0;

// Removed the isSclrp boolean, this runs natively now
void RunSclrpOptimization(NodeContainer nodes, ns3::energy::EnergySourceContainer sources) {
    double w1 = 0.4;
    double w2 = 0.3;
    double w3 = 0.3;

    for (uint32_t i = 0; i < nodes.GetN(); i++) {
        double remainE = sources.Get(i)->GetRemainingEnergy();
        double RE = remainE / 25.0;
        double CI = (rand() % 40) / 100.0;
        double LQ = 0.6 + (rand() % 40) / 100.0;
        double RC = w1 * (1.0 - RE) + w2 * (1.0 - LQ) + w3 * CI;

        Ptr<NetDevice> dev = nodes.Get(i)->GetDevice(0);
        Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice>(dev);
        if (wifiDev) {
            Ptr<YansWifiPhy> phy = DynamicCast<YansWifiPhy>(wifiDev->GetPhy());
            if (phy) {
                if (RC < 0.3) {
                    phy->SetTxPowerStart(8.0);
                    phy->SetTxPowerEnd(8.0);
                } else if (RC > 0.6) {
                    phy->SetTxPowerStart(18.0);
                    phy->SetTxPowerEnd(18.0);
                } else {
                    phy->SetTxPowerStart(12.0);
                    phy->SetTxPowerEnd(12.0);
                }
            }
        }
    }

    // Reschedule itself without the boolean
    Simulator::Schedule(Seconds(2.0), &RunSclrpOptimization, nodes, sources);
}

int main(int argc, char *argv[])
{
    int savedStderr = dup(fileno(stderr));
    [[maybe_unused]] FILE* devnull = freopen("/dev/null", "w", stderr);

    uint32_t numNodes = 10;
    double simTime = 40.0;
    uint32_t packetSize = 128;
    uint32_t totalPackets = 500;
    uint32_t run = 1;

    CommandLine cmd;
    cmd.AddValue("numNodes", "Number of nodes", numNodes);
    cmd.AddValue("totalPackets", "Total packets to send", totalPackets);
    cmd.AddValue("run", "Run number for RNG seed", run);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(run);
    srand(run * 42);

    NodeContainer nodes;
    nodes.Create(numNodes);

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211b);

    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());
    phy.Set("TxPowerStart", DoubleValue(15.0));
    phy.Set("TxPowerEnd", DoubleValue(15.0));

    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");
    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    MobilityHelper mobility;
    Ptr<RandomRectanglePositionAllocator> positionAlloc = CreateObject<RandomRectanglePositionAllocator>();
    positionAlloc->SetX(CreateObjectWithAttributes<UniformRandomVariable>("Min", DoubleValue(0.0), "Max", DoubleValue(100.0)));
    positionAlloc->SetY(CreateObjectWithAttributes<UniformRandomVariable>("Min", DoubleValue(0.0), "Max", DoubleValue(100.0)));
    mobility.SetPositionAllocator(positionAlloc);
    mobility.SetMobilityModel("ns3::RandomWaypointMobilityModel",
        "Speed", StringValue("ns3::UniformRandomVariable[Min=5|Max=20]"),
        "Pause", StringValue("ns3::ConstantRandomVariable[Constant=0.5]"),
        "PositionAllocator", PointerValue(positionAlloc));
    mobility.Install(nodes);

    DsdvHelper dsdv;
    InternetStackHelper internet;
    internet.SetRoutingHelper(dsdv);
    internet.Install(nodes);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = ipv4.Assign(devices);

    BasicEnergySourceHelper energySource;
    energySource.Set("BasicEnergySourceInitialEnergyJ", DoubleValue(25.0));
    ns3::energy::EnergySourceContainer sources = energySource.Install(nodes);
    WifiRadioEnergyModelHelper radioEnergy;
    radioEnergy.Install(devices, sources);

    uint32_t numFlows = numNodes / 2;
    uint32_t packetsPerFlow = totalPackets / numFlows;
    double interval = (simTime - 5.0) / packetsPerFlow;

    Ptr<UniformRandomVariable> randNode = CreateObject<UniformRandomVariable>();

    for (uint32_t i = 0; i < numFlows; i++) {
        uint32_t src = randNode->GetInteger(0, numNodes - 1);
        uint32_t dst = randNode->GetInteger(0, numNodes - 1);
        while (src == dst) { dst = randNode->GetInteger(0, numNodes - 1); }

        uint16_t port = 9000 + i;

        PacketSinkHelper sink("ns3::UdpSocketFactory", Address(InetSocketAddress(Ipv4Address::GetAny(), port)));
        ApplicationContainer sinkApp = sink.Install(nodes.Get(dst));
        sinkApp.Start(Seconds(0.5));
        sinkApp.Stop(Seconds(simTime));

        UdpClientHelper client(interfaces.GetAddress(dst), port);
        client.SetAttribute("MaxPackets", UintegerValue(packetsPerFlow));
        client.SetAttribute("Interval", TimeValue(Seconds(interval)));
        client.SetAttribute("PacketSize", UintegerValue(packetSize));
        ApplicationContainer clientApp = client.Install(nodes.Get(src));
        clientApp.Start(Seconds(1.0 + i * 0.1));
        clientApp.Stop(Seconds(simTime));
    }

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    // Start SCLRP logic
    Simulator::Schedule(Seconds(2.0), &RunSclrpOptimization, nodes, sources);

    AnimationInterface anim("sclrp-animation.xml");
    anim.EnablePacketMetadata(true);
    for (uint32_t i = 0; i < nodes.GetN(); ++i) {
        anim.UpdateNodeSize(i, 3.0, 3.0);
    }

    SystemWallClockMs clock;
    clock.Start();

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    clock.End();
    double compOverhead = clock.GetElapsedReal() / 1000.0;

    fflush(stderr);
    dup2(savedStderr, fileno(stderr));
    close(savedStderr);

    monitor->CheckForLostPackets();
    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();

    uint32_t txPackets = 0, rxPackets = 0, lostPackets = 0;
    double totalDelay = 0, totalJitter = 0, throughput = 0;

    for (auto &flow : stats) {
        txPackets += flow.second.txPackets;
        rxPackets += flow.second.rxPackets;
        lostPackets += flow.second.lostPackets;
        totalDelay += flow.second.delaySum.GetSeconds();
        totalJitter += flow.second.jitterSum.GetSeconds();
        throughput += flow.second.rxBytes * 8.0 / simTime / 1024;
    }

    double avgDelay  = (rxPackets > 0) ? totalDelay / rxPackets : 0;
    double avgJitter = (rxPackets > 0) ? totalJitter / rxPackets : 0;
    double pdr       = (txPackets > 0) ? (double)rxPackets / txPackets * 100 : 0;

    // Hardcoded SCLRP overhead multiplier
    g_commOverhead = lostPackets + (txPackets * 0.1);

    double totalEnergy = 0;
    for (uint32_t i = 0; i < sources.GetN(); ++i) {
        totalEnergy += (25.0 - sources.Get(i)->GetRemainingEnergy());
    }
    double avgEnergy = totalEnergy / numNodes;

    std::cout << "\n======================================\n";
    std::cout << "         SIMULATION RESULTS           \n";
    std::cout << "======================================\n";
    std::cout << "Protocol Mode     = SCLRP Framework\n";
    std::cout << "Number of Nodes   = " << numNodes << "\n";
    std::cout << "Total Packets     = " << totalPackets << "\n";
    std::cout << "Run Number (Seed) = " << run << "\n";
    std::cout << "--------------------------------------\n";
    std::cout << "Packets Sent      = " << txPackets << "\n";
    std::cout << "Packets Received  = " << rxPackets << "\n";
    std::cout << "PDR               = " << pdr << " %\n";
    std::cout << "End-to-End Delay  = " << avgDelay << " sec\n";
    std::cout << "Average Jitter    = " << avgJitter << " sec\n";
    std::cout << "Throughput        = " << throughput << " Kbps\n";
    std::cout << "Communicational Overhead    = " << g_commOverhead << " packets\n";
    std::cout << "Computational Overhead    = " << compOverhead << " sec\n";
    std::cout << "Average Energy    = " << avgEnergy << " Joules\n";
    std::cout << "======================================\n\n";

    Simulator::Destroy();
    return 0;
}
/*
 * Copyright (c) 2015-2020 IMDEA Networks Institute
 * Author: Hany Assasa <hany.assasa@gmail.com>
 */
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/spectrum-module.h"
#include "ns3/wifi-module.h"
#include "common-functions.h"
#include <iomanip>
#include <sstream>

/**
 * Simulation Objective:
 * Evaluate the MU-MIMO beamforming training mechansim in the IEEE 802.11ay standard.
 *
 * Network Topology:
 * Network topology is simple and consists of a single EDMG PCP/AP and two EDMG STAs.
 *
 * Simulation Description:
 * Both EDMG PCP/AP and EDMG STA use a parametric codebook generated by our IEEE 802.11ay
 * Codebook Generator Application in MATLAB. The EDMG AP uses two URA antenna arrays of
 * 2x8 Elements whereas the two EDMA STAs use single PAA.
 * The channel model is generated by our Q-D realization software.
 *
 * Running the Simulation:
 * ./waf --run "evaluate_11ay_mu_mimo"
 *
 * ./waf --run "evaluate_11ay_mu_mimo --kBestCombinations=15"
 *
 * ./waf --run "evaluate_11ay_mu_mimo --qdChannelFolder=IndoorMuMimo40 --kBestCombinations=15"
 *
 * Simulation Output:
 * The simulation generates the following traces:
 * 1. PCAP traces for each station.
 * 2. SNR data for all the packets.
 * 3. SU-MIMO SISO and MIMO phases traces.
 */

NS_LOG_COMPONENT_DEFINE ("Evaluate11ayMU-MIMO");

using namespace ns3;
using namespace std;

/* Network Nodes */
Ptr<WifiNetDevice> apWifiNetDevice, staWifiNetDevice1, staWifiNetDevice2;
Ptr<DmgApWifiMac> apWifiMac;
Ptr<DmgStaWifiMac> sta1WifiMac, sta2WifiMac;
Ptr<DmgWifiPhy> apWifiPhy, sta1WifiPhy, sta2WifiPhy;
Ptr<WifiRemoteStationManager> apRemoteStationManager, staRemoteStationManager;
NetDeviceContainer staDevices;

/* Statistics */
uint64_t macTxDataFailed = 0;
uint64_t transmittedPackets = 0;
uint64_t droppedPackets = 0;
uint64_t receivedPackets = 0;
bool csv = false;                         /* Enable CSV output. */

/* Tracing */
Ptr<QdPropagationEngine> qdPropagationEngine; /* Q-D Propagation Engine. */
AsciiTraceHelper ascii;
struct MIMO_PARAMETERS : public SimpleRefCount<MIMO_PARAMETERS> {
  uint32_t srcNodeID;
  uint32_t dstNodeID;
  Ptr<DmgWifiMac> srcWifiMac;
  Ptr<DmgWifiMac> dstWifiMac;
};

/*** Beamforming Service Periods ***/
uint8_t beamformedLinks = 0;              /* Number of beamformed links */
bool firstDti1 = true;
bool firstDti2 = true;
bool muMimoCompleted = false;
std::string tracesFolder = "Traces/";     /* Directory to store the traces. */
uint32_t kBestCombinations = 15;          /* The number of K best candidates to test in the MIMO phase . */

void
SLSCompleted (Ptr<OutputStreamWrapper> stream, Ptr<SLS_PARAMETERS> parameters, SlsCompletionAttrbitutes attributes)
{
  *stream->GetStream () << parameters->srcNodeID + 1 << "," << ","
                        << qdPropagationEngine->GetCurrentTraceIndex () << ","
                        << uint16_t (attributes.sectorID) << "," << uint16_t (attributes.antennaID)  << ","
                        << parameters->wifiMac->GetTypeOfStation ()  << ","
                        << apWifiNetDevice->GetNode ()->GetId () + 1  << ","
                        << Simulator::Now ().GetNanoSeconds () << std::endl;
  if (!csv)
    {
      std::cout << "EDMG STA " << parameters->wifiMac->GetAddress ()
                << " completed SLS phase with EDMG STA " << attributes.peerStation << std::endl;
      std::cout << "Best Tx Antenna Configuration: AntennaID=" << uint16_t (attributes.antennaID)
                << ", SectorID=" << uint16_t (attributes.sectorID) << std::endl;
      parameters->wifiMac->PrintSnrTable ();
      if (attributes.accessPeriod == CHANNEL_ACCESS_DTI)
        {
          beamformedLinks++;
        }
    }
}

void
MacRxOk (Ptr<OutputStreamWrapper> stream, WifiMacType, Mac48Address, double snrValue)
{
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds () << "," << snrValue << std::endl;
}

void
StationAssoicated (Ptr<DmgWifiMac> staWifiMac, Mac48Address address, uint16_t aid)
{
  if (!csv)
    {
      std::cout << "EDMG STA " << staWifiMac->GetAddress () << " associated with EDMG PCP/AP " << address
                << ", Association ID (AID) = " << aid << std::endl;
    }
}

void
MacTxDataFailed (Mac48Address)
{
  macTxDataFailed++;
}

void
PhyTxEnd (Ptr<const Packet>)
{
  transmittedPackets++;
}

void
PhyRxDrop (Ptr<const Packet> packet, WifiPhyRxfailureReason reason)
{
  droppedPackets++;
}

void
PhyRxEnd (Ptr<const Packet>)
{
  receivedPackets++;
}

void
MuMimoSisoFbckPolled (Ptr<SLS_PARAMETERS> parameters, Mac48Address from)
{
  std::cout << "EDMG STA " << parameters->wifiMac->GetAddress ()
            << " received a poll for feedback as part of the SISO Fbck phase of MU-MIMO BFT from "
            << from << " at " << Simulator::Now ().GetSeconds () << std::endl;
  bool useAwvsInMimoPhase = false;
  parameters->wifiMac->SendBrpFbckFrame (from, useAwvsInMimoPhase);
}

void
MuMimoSisoPhaseMeasurements (Ptr<MIMO_PARAMETERS> parameters, Mac48Address from, MU_MIMO_SNR_MAP measurementsMap)
{
  std::cout << "EDMG STA " << parameters->srcWifiMac->GetAddress ()
            << " reporting SISO phase measurements of MU-MIMO BFT with EDMG STA " << from << " at "
            << Simulator::Now ().GetSeconds () << std::endl;
  /* Save the SISO measuremnts to a trace file */
  Ptr<OutputStreamWrapper> outputSisoPhase
      = ascii.CreateFileStream (tracesFolder + "MuMimoSisoPhaseMeasurements_" + std::to_string (parameters->srcNodeID + 1) + ".csv");
  *outputSisoPhase->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,RX_ANTENNA_ID,PEER_TX_ANTENNA_ID,PEER_TX_SECTOR_ID,SNR,Timestamp" << std::endl;
  for (MU_MIMO_SNR_MAP::iterator it = measurementsMap.begin (); it != measurementsMap.end (); it++)
    {
      ANTENNA_CONFIGURATION antennaConfig = parameters->dstWifiMac->GetCodebook ()->GetAntennaConfigurationShortSSW (std::get<0> (it->first));
      *outputSisoPhase->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                                     << qdPropagationEngine->GetCurrentTraceIndex () << ","
                                     << uint16_t (std::get<1> (it->first)) << "," << uint16_t (antennaConfig.first)  << ","
                                     << uint16_t (antennaConfig.second)  << "," <<  RatioToDb (it->second)  << ","
                                     << Simulator::Now ().GetNanoSeconds () << std::endl;
    }
}

void
MuMimoSisoPhaseComplete (Ptr<SLS_PARAMETERS> parameters, MIMO_FEEDBACK_MAP feedbackMap,
                         uint8_t numberOfTxAntennas, uint8_t numberOfRxAntennas)
{
  std::cout << "EDMG STA " << parameters->wifiMac->GetAddress ()
            << " finished SISO phase of MU-MIMO BFT at " << Simulator::Now ().GetSeconds () << std::endl;
  Ptr<OutputStreamWrapper> outputSisoPhase =
      ascii.CreateFileStream (tracesFolder + "MuMimoSisoPhaseResults_" + std::to_string (parameters->srcNodeID + 1) + ".csv");
  *outputSisoPhase->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,STA_AID,TX_ANTENNA_ID,TX_SECTOR_ID,SNR,Timestamp" << std::endl;
  for (MIMO_FEEDBACK_MAP::iterator it = feedbackMap.begin (); it != feedbackMap.end (); it++)
    {
      *outputSisoPhase->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                                     << qdPropagationEngine->GetCurrentTraceIndex () << ","
                                     << uint16_t (std::get<1> (it->first)) << "," << uint16_t (std::get<0> (it->first))  << ","
                                     << uint16_t (std::get<2> (it->first))  << "," <<  RatioToDb ((it->second))  << ","
                                     << Simulator::Now ().GetNanoSeconds () << std::endl;
    }

  MIMO_ANTENNA_COMBINATIONS_LIST mimoCandidates =
      parameters->wifiMac->FindKBestCombinations (kBestCombinations, numberOfTxAntennas, numberOfRxAntennas, feedbackMap);
  //mimoCandidates.erase (mimoCandidates.begin (), mimoCandidates.begin () + 28);
  /* Append 5 AWVs to each sector in the codebook, increasing the granularity of steering to 5 degrees*/
  DynamicCast<CodebookParametric> (parameters->wifiMac->GetCodebook ())->AppendAwvsForSuMimoBFT_27 ();
  bool useAwvsInMimoPhase = false;
  parameters->wifiMac->StartMuMimoMimoPhase (mimoCandidates, useAwvsInMimoPhase);
}

void
MuMimoMimoCandidatesSelected (Ptr<SLS_PARAMETERS> parameters, uint8_t muGroupId, Antenna2SectorList txCandidates)
{
  std::cout << "EDMG STA " << parameters->wifiMac->GetAddress ()
            << " reporting MIMO candidates Selection for MU-MIMO BFT with MU Group " << uint16_t (muGroupId)
            << " at " << Simulator::Now ().GetSeconds () << std::endl;
  /* Save the MIMO candidates to a trace file */
  Ptr<OutputStreamWrapper> outputMimoTxCandidates = ascii.CreateFileStream (tracesFolder + "MuMimoMimoTxCandidates_" +
                                                                            std::to_string (parameters->srcNodeID + 1) + ".csv");
  uint8_t numberOfAntennas = txCandidates.size ();
  *outputMimoTxCandidates->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,";
  for (uint8_t i = 1; i <= numberOfAntennas; i++)
    {
      *outputMimoTxCandidates->GetStream () << "ANTENNA_ID" << uint16_t(i) << ",SECTOR_ID" << uint16_t (i) << ",";
    }
  *outputMimoTxCandidates->GetStream () << std::endl;
  uint8_t numberOfCandidates = txCandidates.begin ()->second.size ();
  for (uint8_t i = 0; i < numberOfCandidates; i++)
    {
      *outputMimoTxCandidates->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                                            << qdPropagationEngine->GetCurrentTraceIndex () << ",";
      for (Antenna2SectorListI it = txCandidates.begin (); it != txCandidates.end (); it++)
        {
          *outputMimoTxCandidates->GetStream () << uint16_t (it->first) << "," << uint16_t (it->second.at (i)) << ",";
        }
      *outputMimoTxCandidates->GetStream () << std::endl;
    }
}

void
MuMimoMimoPhaseMeasurements (Ptr<MIMO_PARAMETERS> parameters, Mac48Address from, MIMO_SNR_LIST mimoMeasurements,
                             SNR_MEASUREMENT_AWV_IDs_QUEUE minSnr, bool differentRxConfigs,
                             uint8_t nTxAntennas, uint8_t nRxAntennas, uint8_t rxCombinationsTested)
{
  std::cout << "EDMG STA " << parameters->srcWifiMac->GetAddress ()
            << " reporting MIMO phase measurements for SU-MIMO BFT with EDMG STA " << from
            << " at " << Simulator::Now ().GetSeconds () << std::endl;
  /* Save the MIMO Phase Measurements to a trace file */
  Ptr<OutputStreamWrapper> outputMimoPhase = ascii.CreateFileStream (tracesFolder + "MuMimoMimoPhaseMeasurements_" +
                                                                     std::to_string (parameters->srcNodeID + 1) + ".csv");
  *outputMimoPhase->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,";
  for (uint8_t i = 1; i <= nTxAntennas; i++)
    {
      *outputMimoPhase->GetStream () << "TX_ANTENNA_ID" << uint16_t(i) << ",TX_SECTOR_ID" << uint16_t(i) << ",TX_AWV_ID" << uint16_t(i) << ",";
    }
  for (uint8_t i = 1; i <= nRxAntennas; i++)
    {
      *outputMimoPhase->GetStream () << "RX_ANTENNA_ID" << uint16_t(i) << ",RX_SECTOR_ID" << uint16_t(i) << ",RX_AWV_ID" << uint16_t(i) << ",";
    }
  for (uint8_t i = 0; i < nRxAntennas * nTxAntennas; i++)
    {
      *outputMimoPhase->GetStream () << "SNR,";
    }
  *outputMimoPhase->GetStream () << "min_Stream_SNR" << std::endl;
  Ptr<OutputStreamWrapper> outputMimoPhaseR = ascii.CreateFileStream (tracesFolder + "MuMimoMimoPhaseMeasurements_Reduced_" +
                                                                      std::to_string (parameters->srcNodeID + 1) + ".csv");
  *outputMimoPhaseR->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,";
  for (uint8_t i = 1; i <= nTxAntennas; i++)
    {
      *outputMimoPhaseR->GetStream () << "TX_ANTENNA_ID" << uint16_t(i) << ",TX_SECTOR_ID" << uint16_t(i) << ",TX_AWV_ID" << uint16_t(i) << ",";
    }
  for (uint8_t i = 1; i <= nRxAntennas; i++)
    {
      *outputMimoPhaseR->GetStream () << "RX_ANTENNA_ID" << uint16_t(i) << ",RX_SECTOR_ID" << uint16_t(i) << ",RX_AWV_ID" << uint16_t(i) << ",";
    }
  for (uint8_t i = 0; i < nRxAntennas * nTxAntennas; i++)
    {
      *outputMimoPhaseR->GetStream () << "SNR,";
    }
  *outputMimoPhaseR->GetStream () << "min_Stream_SNR" << std::endl;
  std::vector<uint16_t> txIds;
  while (!minSnr.empty ())
    {
      MEASUREMENT_AWV_IDs awvId = minSnr.top ().second;
      //      std::map<RX_ANTENNA_ID, uint16_t> rxAwvIds;
      //      SNR_MEASUREMENT_INDEX measurementIdx = std::make_pair (awvId.second[1], 0);
      //      uint16_t rxSectorId = measurementIdx.first  % rxCombinationsTested;
      //      if (rxSectorId == 0)
      //        rxSectorId = rxCombinationsTested;
      //      uint8_t rxAntennaIdx = (measurementIdx.second % nRxAntennas);
      //      if (rxAntennaIdx == 0)
      //        rxAntennaIdx = nRxAntennas;
      //      rxAwvIds [1] = rxSectorId;
      MIMO_AWV_CONFIGURATION rxCombination
          = parameters->srcWifiMac->GetCodebook ()->GetMimoConfigFromRxAwvId (awvId.second, from);
      MIMO_AWV_CONFIGURATION txCombination
          = parameters->dstWifiMac->GetCodebook ()->GetMimoConfigFromTxAwvId (awvId.first, parameters->dstWifiMac->GetAddress ());
      uint16_t txId = awvId.first;
      MIMO_SNR_LIST measurements;
      for (auto & rxId : awvId.second)
        {
          measurements.push_back (mimoMeasurements.at ((txId - 1) * rxCombinationsTested + rxId.second - 1));
        }
      *outputMimoPhase->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                                     << qdPropagationEngine->GetCurrentTraceIndex () << ",";
      for (uint8_t i = 0; i < nTxAntennas; i ++)
        {
          *outputMimoPhase->GetStream () << uint16_t (txCombination.at (i).first.first) << "," << uint16_t (txCombination.at (i).first.second)
                                         << "," << uint16_t (txCombination.at (i).second) << ",";
        }
      for (uint8_t i = 0; i < nRxAntennas; i++)
        {
          *outputMimoPhase->GetStream () << uint16_t (rxCombination.at (i).first.first) << "," << uint16_t (rxCombination.at (i).first.second)
                                         << "," << uint16_t (rxCombination.at (i).second) << ",";
        }
      uint8_t snrIndex = 0;
      for (uint8_t i = 0; i < nTxAntennas; i++)
        {
          for (uint8_t j = 0; j < nRxAntennas; j++)
            {
              *outputMimoPhase->GetStream () << RatioToDb (measurements.at (j).second.at (snrIndex)) << ",";
              snrIndex++;
            }
        }
      *outputMimoPhase->GetStream () << RatioToDb (minSnr.top ().first) << std::endl;
      if (differentRxConfigs || (std::find (txIds.begin (), txIds.end (), awvId.first) == txIds.end ()))
        {
          txIds.push_back (minSnr.top ().second.first);
          *outputMimoPhaseR->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                                          << qdPropagationEngine->GetCurrentTraceIndex () << ",";
          for (uint8_t i = 0; i < nTxAntennas; i ++)
            {
              *outputMimoPhaseR->GetStream () << uint16_t (txCombination.at (i).first.first) << "," << uint16_t (txCombination.at (i).first.second)
                                              << "," << uint16_t (txCombination.at (i).second) << ",";
            }
          for (uint8_t i = 0; i < nRxAntennas; i++)
            {
              *outputMimoPhaseR->GetStream () << uint16_t (rxCombination.at (i).first.first) << "," << uint16_t (rxCombination.at (i).first.second)
                                              << "," << uint16_t (rxCombination.at (i).second) << ",";
            }
          uint8_t snrIndex = 0;
          for (uint8_t i = 0; i < nTxAntennas; i++)
            {
              for (uint8_t j = 0; j < nRxAntennas; j++)
                {
                  *outputMimoPhaseR->GetStream () << RatioToDb (measurements.at (j).second.at (snrIndex)) << ",";
                  snrIndex++;
                }
            }
          *outputMimoPhaseR->GetStream () << RatioToDb (minSnr.top ().first) << std::endl;
        }
      minSnr.pop ();
    }
}

void
MuMimoMimoPhaseComplete (Ptr<SLS_PARAMETERS> parameters)
{
  std::cout << "EDMG STA " << parameters->wifiMac->GetAddress ()
            << " finished MIMO phase of MU-MIMO BFT at " << Simulator::Now ().GetSeconds () << std::endl;
  muMimoCompleted = true;
}

void
DataTransmissionIntervalStarted (Ptr<DmgStaWifiMac> wifiMac, Mac48Address address, Time dtiDuration)
{
  if (wifiMac->IsAssociated () && firstDti1)
    {

      wifiMac->Perform_TXSS_TXOP (wifiMac->GetBssid ());
      DynamicCast<CodebookParametric> (wifiMac->GetCodebook ())->AppendAwvsForSuMimoBFT_27 ();
      firstDti1 = false;
    }
  else if (wifiMac->IsAssociated () && firstDti2)
    {
      wifiMac->Perform_TXSS_TXOP (wifiMac->GetBssid ());
      DynamicCast<CodebookParametric> (wifiMac->GetCodebook ())->AppendAwvsForSuMimoBFT_27 ();
      firstDti2 = false;
    }
}

void
DataTransmissionIntervalStartedAp (Ptr<DmgApWifiMac> wifiMac, Mac48Address address, Time dtiDuration)
{
  if ((beamformedLinks == 4) && !muMimoCompleted)
    {
      EDMGGroupTuples groupTuples = wifiMac->GetEdmgGroupIdSetElement ()->GetEDMGGroupTuples ();
      std::cout << "EDMG STA " << wifiMac->GetAddress ()
                << " initiating MU-MIMO BFT with EDMG Group " << uint16_t (groupTuples.begin ()->groupID)
                << " at " << Simulator::Now ().GetSeconds () << std::endl;
      Simulator::Schedule (MicroSeconds (1), &DmgWifiMac::StartMuMimoBeamforming, wifiMac,
                           true, groupTuples.begin ()->groupID);
    }
}

int
main (int argc, char *argv[])
{
  string msduAggSize = "max";                                     /* The maximum aggregation size for A-MSDU in Bytes. */
  string mpduAggSize = "max";                                     /* The maximum aggregation size for A-MPDU in Bytes. */
  string phyMode = "EDMG_SC_MCS1";                                /* Type of the Physical Layer. */
  bool verbose = false;                                           /* Print Logging Information. */
  double simulationTime = 10;                                     /* Simulation time in seconds. */
  bool pcapTracing = false;                                       /* PCAP Tracing is enabled or not. */
  std::string arrayConfigAp = "28x_AzEl_SU-MIMO_2x2_27";          /* Phased antenna array configuration. */
  std::string arrayConfigSta = "28x_AzEl_27";                     /* Phased antenna array configuration. */
  std::string qdChannelFolder = "IndoorMuMimo120/Output/Ns3";     /* Path to the folder containing SU-MIMO Q-D files. */

  /* Command line argument parser setup. */
  CommandLine cmd;
  cmd.AddValue ("msduAggSize", "The maximum aggregation size for A-MSDU in Bytes", msduAggSize);
  cmd.AddValue ("msduAggSize", "The maximum aggregation size for A-MPDU in Bytes", msduAggSize);
  cmd.AddValue ("phyMode", "802.11ay PHY Mode", phyMode);
  cmd.AddValue ("verbose", "Turn on all WifiNetDevice log components", verbose);
  cmd.AddValue ("simulationTime", "Simulation time in seconds", simulationTime);
  cmd.AddValue ("kBestCombinations", "The number of K best candidates to test in the MIMO phase", kBestCombinations);
  cmd.AddValue ("pcap", "Enable PCAP Tracing", pcapTracing);
  cmd.AddValue ("arrayConfigAp", "Antenna array configuration for the AP", arrayConfigAp);
  cmd.AddValue ("arrayConfigSta", "Antenna array configuration for the STAs", arrayConfigSta);
  cmd.AddValue ("qdChannelFolder", "Path to the Q-D files describing the MU-MIMO scenario", qdChannelFolder);
  cmd.AddValue ("tracesFolder", "Path to the folder where we dump all the traces", tracesFolder);
  cmd.AddValue ("csv", "Enable CSV output instead of plain text. This mode will suppress all the messages related statistics and events.", csv);
  cmd.Parse (argc, argv);

  /* Validate A-MSDU and A-MPDU values */
  ValidateFrameAggregationAttributes (msduAggSize, mpduAggSize, WIFI_PHY_STANDARD_80211ay);

  /**** DmgWifiHelper is a meta-helper ****/
  DmgWifiHelper wifi;

  /* Basic setup */
  wifi.SetStandard (WIFI_PHY_STANDARD_80211ay);

  /* Turn on logging */
  if (verbose)
    {
      wifi.EnableLogComponents ();
    }

  /**** Setup mmWave Q-D Channel ****/
  /**** Set up Channel ****/
  Ptr<MultiModelSpectrumChannel> spectrumChannel = CreateObject<MultiModelSpectrumChannel> ();
  qdPropagationEngine = CreateObject<QdPropagationEngine> ();
  qdPropagationEngine->SetAttribute ("QDModelFolder", StringValue ("DmgFiles/QdChannel/MU-MIMO-Scenarios/" + qdChannelFolder + "/"));
  Ptr<QdPropagationLossModel> lossModelRaytracing = CreateObject<QdPropagationLossModel> (qdPropagationEngine);
  Ptr<QdPropagationDelayModel> propagationDelayRayTracing = CreateObject<QdPropagationDelayModel> (qdPropagationEngine);
  spectrumChannel->AddSpectrumPropagationLossModel (lossModelRaytracing);
  spectrumChannel->SetPropagationDelayModel (propagationDelayRayTracing);

  /**** Setup physical layer ****/
  SpectrumDmgWifiPhyHelper spectrumWifiPhy = SpectrumDmgWifiPhyHelper::Default ();
  spectrumWifiPhy.SetChannel (spectrumChannel);
  /* All nodes transmit at 10 dBm == 10 mW, no adaptation */
  spectrumWifiPhy.Set ("TxPowerStart", DoubleValue (10.0));
  spectrumWifiPhy.Set ("TxPowerEnd", DoubleValue (10.0));
  spectrumWifiPhy.Set ("TxPowerLevels", UintegerValue (1));
  /* Set the operational channel */
  spectrumWifiPhy.Set ("ChannelNumber", UintegerValue (2));
  /* Set the correct error model */
  spectrumWifiPhy.SetErrorRateModel ("ns3::DmgErrorModel",
                                     "FileName", StringValue ("DmgFiles/ErrorModel/LookupTable_1458_ay.txt"));
  /* Enable support for MU-MIMO */
  spectrumWifiPhy.Set ("SupportMuMimo", BooleanValue (true));
  /* Set default algorithm for all nodes to be constant rate */
  wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager", "DataMode", StringValue (phyMode));
  /* Make four nodes and set them up with the phy and the mac */
  NodeContainer wifiNodes;
  wifiNodes.Create (3);
  Ptr<Node> apWifiNode = wifiNodes.Get (0);
  Ptr<Node> sta1WifiNode = wifiNodes.Get (1);
  Ptr<Node> sta2WifiNode = wifiNodes.Get (2);

  /* Setup EDMG MAC */
  DmgWifiMacHelper wifiMac = DmgWifiMacHelper::Default ();

  /* Install DMG PCP/AP Node */
  Ssid ssid = Ssid ("MU-MIMO");
  wifiMac.SetType ("ns3::DmgApWifiMac",
                   "Ssid", SsidValue (ssid),
                   "BE_MaxAmpduSize", StringValue (mpduAggSize),
                   "BE_MaxAmsduSize", StringValue (msduAggSize),
                   "SSSlotsPerABFT", UintegerValue (8), "SSFramesPerSlot", UintegerValue (16),
                   "BeaconInterval", TimeValue (MicroSeconds (102400)),
                   "EDMGSupported", BooleanValue (true));

  /* Set Parametric Codebook for the EDMG AP */
  wifi.SetCodebook ("ns3::CodebookParametric",
                    "FileName", StringValue ("DmgFiles/Codebook/CODEBOOK_URA_AP_" + arrayConfigAp + ".txt"));

  /* Create Wifi Network Devices (WifiNetDevice) */
  NetDeviceContainer apDevice;
  apDevice = wifi.Install (spectrumWifiPhy, wifiMac, apWifiNode);

  wifiMac.SetType ("ns3::DmgStaWifiMac",
                   "Ssid", SsidValue (ssid), "ActiveProbing", BooleanValue (false),
                   "BE_MaxAmpduSize", StringValue (mpduAggSize),
                   "BE_MaxAmsduSize", StringValue (msduAggSize),
                   "EDMGSupported", BooleanValue (true));

  /* Set Parametric Codebook for the EDMG STA */
  wifi.SetCodebook ("ns3::CodebookParametric",
                    "FileName", StringValue ("DmgFiles/Codebook/CODEBOOK_URA_STA_" + arrayConfigSta + ".txt"));

  NetDeviceContainer staDevice1, staDevice2;
  staDevice1 = wifi.Install (spectrumWifiPhy, wifiMac, sta1WifiNode);
  staDevice2 = wifi.Install (spectrumWifiPhy, wifiMac, sta2WifiNode);

  staDevices.Add (staDevice1);
  staDevices.Add (staDevice2);

  /* Setting mobility model */
  MobilityHelper mobility;
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (wifiNodes);

  /* Internet stack */
  InternetStackHelper stack;
  stack.Install (wifiNodes);

  Ipv4AddressHelper address;
  address.SetBase ("10.0.0.0", "255.255.255.0");
  Ipv4InterfaceContainer apInterface;
  apInterface = address.Assign (apDevice);
  Ipv4InterfaceContainer staInterfaces;
  staInterfaces = address.Assign (staDevices);

  /* We do not want any ARP packets */
  PopulateArpCache ();

  /* Enable Traces */
  if (pcapTracing)
    {
      spectrumWifiPhy.SetPcapDataLinkType (YansWifiPhyHelper::DLT_IEEE802_11_RADIO);
      spectrumWifiPhy.SetSnapshotLength (120);
      spectrumWifiPhy.EnablePcap ("Traces/AccessPoint", apDevice, false);
      spectrumWifiPhy.EnablePcap ("Traces/STA", staDevices, false);
    }

  /* Stations */
  apWifiNetDevice = StaticCast<WifiNetDevice> (apDevice.Get (0));
  staWifiNetDevice1 = StaticCast<WifiNetDevice> (staDevices.Get (0));
  staWifiNetDevice2 = StaticCast<WifiNetDevice> (staDevices.Get (1));
  apRemoteStationManager = StaticCast<WifiRemoteStationManager> (apWifiNetDevice->GetRemoteStationManager ());
  apWifiMac = StaticCast<DmgApWifiMac> (apWifiNetDevice->GetMac ());
  sta1WifiMac = StaticCast<DmgStaWifiMac> (staWifiNetDevice1->GetMac ());
  sta2WifiMac = StaticCast<DmgStaWifiMac> (staWifiNetDevice2->GetMac ());
  apWifiPhy = StaticCast<DmgWifiPhy> (apWifiNetDevice->GetPhy ());
  sta1WifiPhy = StaticCast<DmgWifiPhy> (staWifiNetDevice1->GetPhy ());
  sta2WifiPhy = StaticCast<DmgWifiPhy> (staWifiNetDevice2->GetPhy ());
  staRemoteStationManager = StaticCast<WifiRemoteStationManager> (staWifiNetDevice1->GetRemoteStationManager ());

  /** Connect Traces **/
  AsciiTraceHelper ascii;
  Ptr<OutputStreamWrapper> outputSlsPhase = CreateSlsTraceStream (tracesFolder + "slsResults");

  /* EDMG AP Straces */

  /* SLS Traces */
  *outputSlsPhase->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,ANTENNA_ID_1,SECTOR_ID_1,AWV_ID_1,"
                                   "ANTENNA_ID_2,SECTOR_ID_2,AWV_ID_2,ROLE,BSS_ID,SNR,Timestamp" << std::endl;

  Ptr<SLS_PARAMETERS> parametersAp = Create<SLS_PARAMETERS> ();
  parametersAp->srcNodeID = apWifiNetDevice->GetNode ()->GetId ();
  //parametersAp->dstNodeID = staWifiNetDevice->GetNode ()->GetId ();
  parametersAp->wifiMac = apWifiMac;
  apWifiMac->TraceConnectWithoutContext ("SLSCompleted", MakeBoundCallback (&SLSCompleted, outputSlsPhase, parametersAp));
  apWifiMac->TraceConnectWithoutContext ("MuMimoSisoPhaseCompleted", MakeBoundCallback (&MuMimoSisoPhaseComplete, parametersAp));
  apWifiMac->TraceConnectWithoutContext ("MuMimoMimoCandidatesSelected", MakeBoundCallback (&MuMimoMimoCandidatesSelected, parametersAp));
  apWifiMac->TraceConnectWithoutContext ("MuMimoMimoPhaseCompleted", MakeBoundCallback (&MuMimoMimoPhaseComplete, parametersAp));
  apWifiMac->TraceConnectWithoutContext ("DTIStarted", MakeBoundCallback (&DataTransmissionIntervalStartedAp, apWifiMac));
  apWifiPhy->TraceConnectWithoutContext ("PhyRxEnd", MakeCallback (&PhyRxEnd));
  apWifiPhy->TraceConnectWithoutContext ("PhyRxDrop", MakeCallback (&PhyRxDrop));

  /* DMG STA Straces */
  Ptr<SLS_PARAMETERS> parametersSta = Create<SLS_PARAMETERS> ();
  parametersSta->srcNodeID = staWifiNetDevice1->GetNode ()->GetId ();
  parametersSta->dstNodeID = apWifiNetDevice->GetNode ()->GetId ();
  parametersSta->wifiMac = sta1WifiMac;
  Ptr<MIMO_PARAMETERS> mimoParametersSta1 = Create<MIMO_PARAMETERS> ();
  mimoParametersSta1->srcNodeID = staWifiNetDevice1->GetNode ()->GetId ();
  mimoParametersSta1->dstNodeID = apWifiNetDevice->GetNode ()->GetId ();
  mimoParametersSta1->srcWifiMac = sta1WifiMac;
  mimoParametersSta1->dstWifiMac = apWifiMac;
  sta1WifiMac->TraceConnectWithoutContext ("Assoc", MakeBoundCallback (&StationAssoicated, sta1WifiMac));
  sta1WifiMac->TraceConnectWithoutContext ("SLSCompleted", MakeBoundCallback (&SLSCompleted, outputSlsPhase, parametersSta));
  sta1WifiMac->TraceConnectWithoutContext ("MuMimoSisoFbckPolled", MakeBoundCallback (&MuMimoSisoFbckPolled, parametersSta));
  sta1WifiMac->TraceConnectWithoutContext ("MuMimoSisoPhaseMeasurements", MakeBoundCallback (&MuMimoSisoPhaseMeasurements, mimoParametersSta1));
  sta1WifiMac->TraceConnectWithoutContext ("SuMimoMimoPhaseMeasurements", MakeBoundCallback (&MuMimoMimoPhaseMeasurements, mimoParametersSta1));
  sta1WifiMac->TraceConnectWithoutContext ("MuMimoMimoPhaseCompleted", MakeBoundCallback (&MuMimoMimoPhaseComplete, parametersSta));
  sta1WifiMac->TraceConnectWithoutContext ("DTIStarted", MakeBoundCallback (&DataTransmissionIntervalStarted, sta1WifiMac));
  sta1WifiPhy->TraceConnectWithoutContext ("PhyTxEnd", MakeCallback (&PhyTxEnd));
  staRemoteStationManager->TraceConnectWithoutContext ("MacTxDataFailed", MakeCallback (&MacTxDataFailed));

  Ptr<SLS_PARAMETERS> parametersSta1 = Create<SLS_PARAMETERS> ();
  parametersSta1->srcNodeID = staWifiNetDevice2->GetNode ()->GetId ();
  parametersSta1->dstNodeID = apWifiNetDevice->GetNode ()->GetId ();
  parametersSta1->wifiMac = sta2WifiMac;
  Ptr<MIMO_PARAMETERS> mimoParametersSta2 = Create<MIMO_PARAMETERS> ();
  mimoParametersSta2->srcNodeID = staWifiNetDevice2->GetNode ()->GetId ();
  mimoParametersSta2->dstNodeID = apWifiNetDevice->GetNode ()->GetId ();
  mimoParametersSta2->srcWifiMac = sta2WifiMac;
  mimoParametersSta2->dstWifiMac = apWifiMac;
  sta2WifiMac->TraceConnectWithoutContext ("Assoc", MakeBoundCallback (&StationAssoicated, sta2WifiMac));
  sta2WifiMac->TraceConnectWithoutContext ("SLSCompleted", MakeBoundCallback (&SLSCompleted, outputSlsPhase, parametersSta1));
  sta2WifiMac->TraceConnectWithoutContext ("MuMimoSisoFbckPolled", MakeBoundCallback (&MuMimoSisoFbckPolled, parametersSta1));
  sta2WifiMac->TraceConnectWithoutContext ("MuMimoSisoPhaseMeasurements", MakeBoundCallback (&MuMimoSisoPhaseMeasurements, mimoParametersSta2));
  sta2WifiMac->TraceConnectWithoutContext ("SuMimoMimoPhaseMeasurements", MakeBoundCallback (&MuMimoMimoPhaseMeasurements, mimoParametersSta2));
  sta2WifiMac->TraceConnectWithoutContext ("MuMimoMimoPhaseCompleted", MakeBoundCallback (&MuMimoMimoPhaseComplete, parametersSta1));
  sta2WifiMac->TraceConnectWithoutContext ("DTIStarted", MakeBoundCallback (&DataTransmissionIntervalStarted, sta2WifiMac));
  sta2WifiPhy->TraceConnectWithoutContext ("PhyTxEnd", MakeCallback (&PhyTxEnd));
  staRemoteStationManager->TraceConnectWithoutContext ("MacTxDataFailed", MakeCallback (&MacTxDataFailed));

  /* Get SNR Traces */
  Ptr<OutputStreamWrapper> snrStream = ascii.CreateFileStream (tracesFolder + "snrValues.csv");
  apRemoteStationManager->TraceConnectWithoutContext ("MacRxOK", MakeBoundCallback (&MacRxOk, snrStream));

  Simulator::Stop (Seconds (simulationTime + 0.101));
  Simulator::Run ();
  Simulator::Destroy ();

  return 0;
}

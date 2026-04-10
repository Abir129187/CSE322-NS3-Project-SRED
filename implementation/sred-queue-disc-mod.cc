#include "sred-queue-disc-mod.h"

#include "ns3/abort.h"
#include "ns3/double.h"
#include "ns3/drop-tail-queue.h"
#include "ns3/enum.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("SredQueueDiscMod");

NS_OBJECT_ENSURE_REGISTERED(SredQueueDiscMod);

TypeId
SredQueueDiscMod::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::SredQueueDiscMod")
            .SetParent<QueueDisc>()
            .SetGroupName("TrafficControl")
            .AddConstructor<SredQueueDiscMod>()
            //  Original SRED attributes
            .AddAttribute("MeanPktSize",
                          "Average of packet size",
                          UintegerValue(500),
                          MakeUintegerAccessor(&SredQueueDiscMod::m_meanPktSize),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("MaxTh",
                          "SRED buffer parameter B (packets or bytes). Forced-drop threshold "
                          "and basis for the three-level p_sred(q) step function.",
                          DoubleValue(25),
                          MakeDoubleAccessor(&SredQueueDiscMod::m_maxTh),
                          MakeDoubleChecker<double>())
            .AddAttribute("PMax",
                          "Maximum early-drop probability in the three-level p_sred(q) "
                          "step function (eq. 4.3).",
                          DoubleValue(0.15),
                          MakeDoubleAccessor(&SredQueueDiscMod::m_pMax),
                          MakeDoubleChecker<double>(0, 1))
            .AddAttribute("MaxSize",
                          "The maximum number of packets accepted by this queue disc",
                          QueueSizeValue(QueueSize("25p")),
                          MakeQueueSizeAccessor(&QueueDisc::SetMaxSize, &QueueDisc::GetMaxSize),
                          MakeQueueSizeChecker())
            .AddAttribute("ZombieListSize",
                          "Number of entries in the zombie list (flow cache)",
                          UintegerValue(1000),
                          MakeUintegerAccessor(&SredQueueDiscMod::m_zombieListSize),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("ZombieOverwriteProb",
                          "Probability of overwriting a zombie entry on a miss",
                          DoubleValue(0.25),
                          MakeDoubleAccessor(&SredQueueDiscMod::m_zombieOverwriteProb),
                          MakeDoubleChecker<double>(0, 1))
            .AddAttribute("HitProbEwma",
                          "EWMA smoothing weight alpha for hit-frequency estimator P(t). "
                          "Default 0.001 = 1/1000.",
                          DoubleValue(0.001),
                          MakeDoubleAccessor(&SredQueueDiscMod::m_hitProbEwma),
                          MakeDoubleChecker<double>(0, 1))
            .AddAttribute("LinkBandwidth",
                          "The SRED link bandwidth",
                          DataRateValue(DataRate("1.5Mbps")),
                          MakeDataRateAccessor(&SredQueueDiscMod::m_linkBandwidth),
                          MakeDataRateChecker())
            .AddAttribute("LinkDelay",
                          "The SRED link delay",
                          TimeValue(MilliSeconds(20)),
                          MakeTimeAccessor(&SredQueueDiscMod::m_linkDelay),
                          MakeTimeChecker())
            .AddAttribute("UseEcn",
                          "True to use ECN (packets are marked instead of being dropped)",
                          BooleanValue(false),
                          MakeBooleanAccessor(&SredQueueDiscMod::m_useEcn),
                          MakeBooleanChecker())
            .AddAttribute("UseHardDrop",
                          "True to always drop packets above max threshold",
                          BooleanValue(true),
                          MakeBooleanAccessor(&SredQueueDiscMod::m_useHardDrop),
                          MakeBooleanChecker())
            .AddAttribute("FullSred",
                          "True to enable Full SRED (hit-multiplier boost eq. 5.1). "
                          "Figure 16 uses this; Figures 10/11 use Simple SRED.",
                          //   BooleanValue(false),
                          BooleanValue(true),
                          MakeBooleanAccessor(&SredQueueDiscMod::m_fullSred),
                          MakeBooleanChecker())
            // -- NEW: Delay-aware attributes --
            .AddAttribute("DelayGamma",
                          "Weight factor (gamma) for the delay penalty term in the "
                          "delay-aware zap probability. Higher gamma = stronger penalty "
                          "for delay-heavy traffic. Default 1.0.",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&SredQueueDiscMod::m_gamma),
                          MakeDoubleChecker<double>(0))
            .AddAttribute("DelayExponentK",
                          "Exponent k for the normalized delay Dn(t)^k. "
                          "k > 1 penalizes large delays super-linearly. Default 2.0.",
                          DoubleValue(2.0),
                          MakeDoubleAccessor(&SredQueueDiscMod::m_delayK),
                          MakeDoubleChecker<double>(0))
            .AddAttribute("DelayReference",
                          "Reference delay Dref for normalization. Dn(t) = min(1, D(t)/Dref). "
                          "Should reflect the acceptable queueing delay target. Default 20ms.",
                          TimeValue(MilliSeconds(20)),
                          MakeTimeAccessor(&SredQueueDiscMod::m_dRef),
                          MakeTimeChecker())
            .AddAttribute("DelayEwmaWeight",
                          "EWMA weight beta for the sojourn-time estimator D(t). "
                          "D(t) = (1-beta)*D(t-1) + beta*sojourn. Default 0.1.",
                          DoubleValue(0.1),
                          MakeDoubleAccessor(&SredQueueDiscMod::m_delayEwmaWeight),
                          MakeDoubleChecker<double>(0, 1));

    return tid;
}

SredQueueDiscMod::SredQueueDiscMod()
    : QueueDisc(QueueDiscSizePolicy::SINGLE_INTERNAL_QUEUE)
{
    NS_LOG_FUNCTION(this);
    m_uv = CreateObject<UniformRandomVariable>();
}

SredQueueDiscMod::~SredQueueDiscMod()
{
    NS_LOG_FUNCTION(this);
}

void
SredQueueDiscMod::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_uv = nullptr;
    m_zombieList.clear();
    QueueDisc::DoDispose();
}

int64_t
SredQueueDiscMod::AssignStreams(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    m_uv->SetStream(stream);
    return 1;
}

double
SredQueueDiscMod::GetFlowCountEstimate() const
{
    if (m_pHit > 0.0)
    {
        return 1.0 / m_pHit;
    }
    return 0.0;
}

Time
SredQueueDiscMod::GetAverageDelay() const
{
    return Seconds(m_avgDelay);
}

// Zombie list logic  (unchanged from original SRED)

bool
SredQueueDiscMod::ZombieListCheck(uint32_t flowHash)
{
    NS_LOG_FUNCTION(this << flowHash);

    // During warm-up: fill new slots while still allowing hit detection.
    if (m_zombiePopulated < m_zombieListSize)
    {
        // for (uint32_t i = 0; i < m_zombiePopulated; i++)
        // {
        //     if (m_zombieList[i].valid && m_zombieList[i].flowHash == flowHash)
        //     {
        //         m_zombieList[i].count++;
        //         m_zombieList[i].timestamp = Simulator::Now();
        //         NS_LOG_DEBUG("Zombie HIT during warm-up at index "
        //                      << i << " count=" << m_zombieList[i].count);
        //         return true;
        //     }
        // }
        // No hit — add as new entry
        m_zombieList[m_zombiePopulated].flowHash = flowHash;
        m_zombieList[m_zombiePopulated].count = 0;
        m_zombieList[m_zombiePopulated].timestamp = Simulator::Now();
        m_zombieList[m_zombiePopulated].valid = true;
        m_zombiePopulated++;
        return false;
    }

    // Pick a random zombie index
    uint32_t idx = static_cast<uint32_t>(m_uv->GetValue(0, m_zombieListSize));
    if (idx >= m_zombieListSize)
    {
        idx = m_zombieListSize - 1;
    }

    if (m_zombieList[idx].valid && m_zombieList[idx].flowHash == flowHash)
    {
        // HIT
        m_zombieList[idx].count++;
        m_zombieList[idx].timestamp = Simulator::Now();
        NS_LOG_DEBUG("Zombie HIT at index " << idx << " count=" << m_zombieList[idx].count);
        return true;
    }
    else
    {
        // MISS — with probability m_zombieOverwriteProb, overwrite the zombie
        double u = m_uv->GetValue();
        if (u < m_zombieOverwriteProb)
        {
            m_zombieList[idx].flowHash = flowHash;
            m_zombieList[idx].count = 0;
            m_zombieList[idx].timestamp = Simulator::Now();
            m_zombieList[idx].valid = true;
        }
        NS_LOG_DEBUG("Zombie MISS at index " << idx);
        return false;
    }
}

// Drop probability computation  ** MODIFIED: Delay-Aware **

double
SredQueueDiscMod::CalculateDropProbability(uint32_t nQueued, bool hit)
{
    NS_LOG_FUNCTION(this << nQueued << hit);

    //  Step 4: Update EWMA estimate of P(t) BEFORE computing drop prob
    double hitVal = hit ? 1.0 : 0.0;
    m_pHit = (1.0 - m_hitProbEwma) * m_pHit + m_hitProbEwma * hitVal;

    //  Step 1: Three-level step function p_sred(q) (eq. 4.3)
    // Unchanged from original SRED.
    double B = m_maxTh;
    double pSred = 0.0;

    if (static_cast<double>(nQueued) >= B / 3.0)
    {
        pSred = m_pMax;
    }
    else if (static_cast<double>(nQueued) >= B / 6.0)
    {
        pSred = m_pMax / 4.0;
    }
    // else pSred remains 0.0

    //  Step 2: DELAY-AWARE zap probability (MODIFIED)
    //
    //   Dn(t) = min(1, D(t) / Dref)          — normalized delay
    //   Pda(t) = p_sred(q) * min(1, 1/(256 * P(t)^2))
    //                       * (1 + gamma * Dn(t)^k)
    //
    // Intuition: When average queue delay D(t) is low relative to the
    // reference threshold Dref, the delay factor (1 + gamma*Dn^k) stays
    // close to 1 and the drop probability is essentially the same as
    // standard SRED. As delay grows toward or beyond Dref, the penalty
    // term increases — super-linearly when k > 1 — causing more aggressive
    // drops to push the queue delay back down. This is especially
    // important for real-time traffic where latency matters more
    // than raw throughput. The gamma parameter controls how strongly
    // the delay signal influences the drop decision.

    // Compute normalized delay Dn(t)
    double Dn = 0.0;
    if (m_dRef.GetSeconds() > 0.0)
    {
        Dn = std::min(1.0, m_avgDelay / m_dRef.GetSeconds());
    }

    // Compute delay penalty factor: (1 + gamma * Dn^k)
    double delayFactor = 1.0 + m_gamma * std::pow(Dn, m_delayK);

    // Compute flow-count scaling with delay penalty
    double pZap;
    if (m_pHit >= (1.0 / 256.0))
    {
        // Normal regime: P(t) >= 1/256
        // Original: min(1, (1/(256*P(t)))^2)
        // Modified: min(1, 1/((256*P(t))^2)) * delayFactor
        double invScale = 1.0 / (256.0 * m_pHit);
        invScale *= invScale; // Square the inverse scale
        pZap = pSred * std::min(1.0, invScale) * delayFactor;
    }
    else
    {
        // Fallback regime: P(t) < 1/256 (including P(t) == 0)
        // Use base p_sred with delay factor
        // cap at pMax,then scale by delay
        // pZap = std::min(m_pMax, pSred) * delayFactor;
        pZap = pSred * delayFactor;
    }

    //  Step 3: Full SRED hit-multiplier (eq. 5.1)
    // Still applied: penalize overactive flows on hit.
    double pDrop = pZap;
    if (m_fullSred && hit && m_pHit > 0.0)
    {
        // cap the multiplier so it can't exceed 2× p_max
        // pDrop = std::min(m_pMax * 2.0, pZap * (1.0 + 1.0 / m_pHit));
        pDrop = pZap * (1.0 + 1.0 / m_pHit);
    }

    // Clamp to [0, 1]
    if (pDrop > 1.0)
    {
        pDrop = 1.0;
    }
    if (pDrop < 0.0)
    {
        pDrop = 0.0;
    }

    m_sredDropProb = pDrop;

    NS_LOG_DEBUG("SRED-MOD prob: pSred=" << pSred << " Dn=" << Dn << " delayFactor=" << delayFactor
                                         << " pZap=" << pZap << " pDrop=" << pDrop
                                         << " P(t)=" << m_pHit
                                         << " 1/P(t)=" << (m_pHit > 0 ? 1.0 / m_pHit : 0.0)
                                         << " avgDelay=" << m_avgDelay << " hit=" << hit);

    return pDrop;
}

// Enqueue

bool
SredQueueDiscMod::DoEnqueue(Ptr<QueueDiscItem> item)
{
    NS_LOG_FUNCTION(this << item);

    uint32_t nQueued = GetInternalQueue(0)->GetCurrentSize().GetValue();

    // Compute flow hash from packet
    uint32_t flowHash = item->Hash();

    // Zombie list comparison
    bool hit = ZombieListCheck(flowHash);

    // Compute drop probability (now uses delay-aware formula)
    double pDrop = CalculateDropProbability(nQueued, hit);

    NS_LOG_DEBUG("\t bytesInQueue " << GetInternalQueue(0)->GetNBytes() << "\t packetsInQueue "
                                    << GetInternalQueue(0)->GetNPackets() << "\t pHit " << m_pHit
                                    << "\t 1/P(t) " << (m_pHit > 0 ? 1.0 / m_pHit : 0.0)
                                    << "\t pDrop " << pDrop << "\t avgDelay " << m_avgDelay);

    uint32_t dropType = DTYPE_NONE;

    // Force drop if queue is at or above the maximum threshold
    if (nQueued >= static_cast<uint32_t>(m_maxTh))
    {
        NS_LOG_DEBUG("adding DROP FORCED MARK");
        dropType = DTYPE_FORCED;
    }
    else
    {
        // Probabilistic (unforced) drop based on delay-aware probability
        double u = m_uv->GetValue();
        if (u < pDrop)
        {
            NS_LOG_LOGIC("SRED-MOD unforced drop: u=" << u << " < pDrop=" << pDrop);
            dropType = DTYPE_UNFORCED;
        }
    }

    if (dropType == DTYPE_UNFORCED)
    {
        if (!m_useEcn || !Mark(item, UNFORCED_MARK))
        {
            NS_LOG_DEBUG("\t Dropping due to SRED-MOD Prob " << pDrop);
            DropBeforeEnqueue(item, UNFORCED_DROP);
            return false;
        }
        NS_LOG_DEBUG("\t Marking due to SRED-MOD Prob " << pDrop);
    }
    else if (dropType == DTYPE_FORCED)
    {
        if (m_useHardDrop || !m_useEcn || !Mark(item, FORCED_MARK))
        {
            NS_LOG_DEBUG("\t Dropping due to Full Queue");
            DropBeforeEnqueue(item, FORCED_DROP);
            return false;
        }
        NS_LOG_DEBUG("\t Marking due to Full Queue");
    }

    bool retval = GetInternalQueue(0)->Enqueue(item);

    NS_LOG_LOGIC("Number packets " << GetInternalQueue(0)->GetNPackets());
    NS_LOG_LOGIC("Number bytes " << GetInternalQueue(0)->GetNBytes());

    return retval;
}

// Dequeue  ** MODIFIED: Update average delay EWMA **

Ptr<QueueDiscItem>
SredQueueDiscMod::DoDequeue()
{
    NS_LOG_FUNCTION(this);

    if (GetInternalQueue(0)->IsEmpty())
    {
        NS_LOG_LOGIC("Queue empty");
        return nullptr;
    }
    else
    {
        Ptr<QueueDiscItem> item = GetInternalQueue(0)->Dequeue();

        //  NEW: Compute sojourn time and update EWMA delay
        // The QueueDisc base class stamps each item with Simulator::Now()
        // at enqueue time (queue-disc.cc line ~851).  We read that
        // timestamp here to compute the actual queuing delay.
        Time sojourn = Simulator::Now() - item->GetTimeStamp();
        double sojournSec = sojourn.GetSeconds();

        // Update EWMA:  D(t) = (1 - beta) * D(t-1) + beta * sojourn
        m_avgDelay = (1.0 - m_delayEwmaWeight) * m_avgDelay + m_delayEwmaWeight * sojournSec;

        NS_LOG_DEBUG("Dequeue sojourn=" << sojournSec << "s  avgDelay=" << m_avgDelay << "s");
        //  END NEW

        NS_LOG_LOGIC("Popped " << item);
        NS_LOG_LOGIC("Number packets " << GetInternalQueue(0)->GetNPackets());
        NS_LOG_LOGIC("Number bytes " << GetInternalQueue(0)->GetNBytes());

        return item;
    }
}

// Peek

Ptr<const QueueDiscItem>
SredQueueDiscMod::DoPeek()
{
    NS_LOG_FUNCTION(this);
    if (GetInternalQueue(0)->IsEmpty())
    {
        NS_LOG_LOGIC("Queue empty");
        return nullptr;
    }

    Ptr<const QueueDiscItem> item = GetInternalQueue(0)->Peek();

    NS_LOG_LOGIC("Number packets " << GetInternalQueue(0)->GetNPackets());
    NS_LOG_LOGIC("Number bytes " << GetInternalQueue(0)->GetNBytes());

    return item;
}

// InitializeParams  ** MODIFIED: Initialize delay state **

void
SredQueueDiscMod::InitializeParams()
{
    NS_LOG_FUNCTION(this);
    NS_LOG_INFO("Initializing Delay-Aware SRED params.");

    // Initialize zombie list
    m_zombieList.resize(m_zombieListSize);
    m_zombiePopulated = 0;

    // Initialize EWMA hit probability estimate
    m_pHit = 0.0;
    m_sredDropProb = 0.0;

    //  NEW: Initialize delay estimator
    m_avgDelay = 0.0;

    NS_LOG_DEBUG("\tm_maxTh (B) " << m_maxTh << "; m_pMax " << m_pMax << "; m_zombieListSize "
                                  << m_zombieListSize << "; m_zombieOverwriteProb "
                                  << m_zombieOverwriteProb << "; m_hitProbEwma " << m_hitProbEwma
                                  << "; m_gamma " << m_gamma << "; m_delayK " << m_delayK
                                  << "; m_dRef " << m_dRef.GetSeconds() << "s"
                                  << "; m_delayEwmaWeight " << m_delayEwmaWeight);
}

// CheckConfig

bool
SredQueueDiscMod::CheckConfig()
{
    NS_LOG_FUNCTION(this);
    if (GetNQueueDiscClasses() > 0)
    {
        NS_LOG_ERROR("SredQueueDiscMod cannot have classes");
        return false;
    }

    if (GetNPacketFilters() > 0)
    {
        NS_LOG_ERROR("SredQueueDiscMod cannot have packet filters");
        return false;
    }

    if (GetNInternalQueues() == 0)
    {
        // add a DropTail queue
        AddInternalQueue(
            CreateObjectWithAttributes<DropTailQueue<QueueDiscItem>>("MaxSize",
                                                                     QueueSizeValue(GetMaxSize())));
    }

    if (GetNInternalQueues() != 1)
    {
        NS_LOG_ERROR("SredQueueDiscMod needs 1 internal queue");
        return false;
    }

    return true;
}

} // namespace ns3

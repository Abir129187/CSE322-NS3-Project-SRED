#include "sred-queue-disc.h"

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

NS_LOG_COMPONENT_DEFINE("SredQueueDisc");

NS_OBJECT_ENSURE_REGISTERED(SredQueueDisc);

TypeId
SredQueueDisc::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::SredQueueDisc")
            .SetParent<QueueDisc>()
            .SetGroupName("TrafficControl")
            .AddConstructor<SredQueueDisc>()
            .AddAttribute("MeanPktSize",
                          "Average of packet size",
                          UintegerValue(500),
                          MakeUintegerAccessor(&SredQueueDisc::m_meanPktSize),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("MaxTh",
                          "SRED buffer parameter B (packets or bytes). This is the "
                          "forced-drop threshold AND the basis for the three-level "
                          "p_sred(q) step function (eq. 4.3: thresholds at B/3 and B/6). "
                          "Note: this is a logical parameter of the SRED algorithm, not "
                          "necessarily the physical buffer capacity (set via MaxSize).",
                          DoubleValue(25),
                          MakeDoubleAccessor(&SredQueueDisc::m_maxTh),
                          MakeDoubleChecker<double>())
            .AddAttribute("PMax",
                          "Maximum early-drop probability used in the three-level "
                          "p_sred(q) step function (eq. 4.3). Applied when q >= B/3.",
                          DoubleValue(0.15),
                          MakeDoubleAccessor(&SredQueueDisc::m_pMax),
                          MakeDoubleChecker<double>(0, 1))
            .AddAttribute("MaxSize",
                          "The maximum number of packets accepted by this queue disc",
                          QueueSizeValue(QueueSize("25p")),
                          MakeQueueSizeAccessor(&QueueDisc::SetMaxSize, &QueueDisc::GetMaxSize),
                          MakeQueueSizeChecker())
            .AddAttribute("ZombieListSize",
                          "Number of entries in the zombie list (flow cache)",
                          UintegerValue(1000),
                          MakeUintegerAccessor(&SredQueueDisc::m_zombieListSize),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("ZombieOverwriteProb", // probability p in the paper (Section III)
                          "Probability of overwriting a zombie entry on a miss",
                          DoubleValue(0.25),
                          MakeDoubleAccessor(&SredQueueDisc::m_zombieOverwriteProb),
                          MakeDoubleChecker<double>(0, 1))
            .AddAttribute("HitProbEwma",
                          "EWMA smoothing weight alpha for hit-frequency estimator P(t). "
                          "The paper (Section III) uses alpha = 1/M where M = ZombieListSize. "
                          "Default 0.001 = 1/1000.",
                          DoubleValue(0.001),
                          MakeDoubleAccessor(&SredQueueDisc::m_hitProbEwma),
                          MakeDoubleChecker<double>(0, 1))
            .AddAttribute("LinkBandwidth",
                          "The SRED link bandwidth",
                          DataRateValue(DataRate("1.5Mbps")),
                          MakeDataRateAccessor(&SredQueueDisc::m_linkBandwidth),
                          MakeDataRateChecker())
            .AddAttribute("LinkDelay",
                          "The SRED link delay",
                          TimeValue(MilliSeconds(20)),
                          MakeTimeAccessor(&SredQueueDisc::m_linkDelay),
                          MakeTimeChecker())
            .AddAttribute("UseEcn",
                          "True to use ECN (packets are marked instead of being dropped)",
                          BooleanValue(false),
                          MakeBooleanAccessor(&SredQueueDisc::m_useEcn),
                          MakeBooleanChecker())
            .AddAttribute("UseHardDrop",
                          "True to always drop packets above max threshold",
                          BooleanValue(true),
                          MakeBooleanAccessor(&SredQueueDisc::m_useHardDrop),
                          MakeBooleanChecker());

    return tid;
}

SredQueueDisc::SredQueueDisc()
    : QueueDisc(QueueDiscSizePolicy::SINGLE_INTERNAL_QUEUE)
{
    NS_LOG_FUNCTION(this);
    m_uv = CreateObject<UniformRandomVariable>();
}

SredQueueDisc::~SredQueueDisc()
{
    NS_LOG_FUNCTION(this);
}

void
SredQueueDisc::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_uv = nullptr;
    m_zombieList.clear();
    QueueDisc::DoDispose();
}

int64_t
SredQueueDisc::AssignStreams(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    m_uv->SetStream(stream);
    return 1;
}

double
SredQueueDisc::GetFlowCountEstimate() const
{
    if (m_pHit > 0.0)
    {
        return 1.0 / m_pHit;
    }
    return 0.0;
}

// -----------------------------------------------------------------------
// Zombie list logic
// -----------------------------------------------------------------------

bool
SredQueueDisc::ZombieListCheck(uint32_t flowHash)
{
    NS_LOG_FUNCTION(this << flowHash);

    // During warm-up: the list is not yet full, so we fill new slots.
    // However, the paper does NOT suppress hit detection during this phase —
    // hits are possible as soon as any entry is in the list.
    if (m_zombiePopulated < m_zombieListSize)
    {
        // // First, scan existing entries for a hit (linear search during warm-up)
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
        // HIT — the arriving packet belongs to the same flow as the zombie
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

// -----------------------------------------------------------------------
// Drop probability computation
// -----------------------------------------------------------------------

double
SredQueueDisc::CalculateDropProbability(uint32_t nQueued, bool hit)
{
    NS_LOG_FUNCTION(this << nQueued << hit);

    // Update EWMA estimate of P(t) immediately upon packet arrival
    double hitVal = hit ? 1.0 : 0.0;
    m_pHit = (1.0 - m_hitProbEwma) * m_pHit + m_hitProbEwma * hitVal;

    // ---- Step 1: Three-level step function p_sred(q) (eq. 4.3) ----
    // B = m_maxTh (SRED buffer parameter).
    // p_sred = p_max           if q >= B/3
    //          p_max / 4       if B/6 <= q < B/3
    //          0               if q < B/6
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
    // else pSred remains 0.0 — no drops when queue is below B/6

    // ---- Step 2: Inverse-square flow-count scaling p_zap (eq. 4.4) ----
    // p_zap = p_sred(q) * min(1, (1 / (256 * P(t)))^2 )
    //
    // Fallback (eq. 4.8): When P(t) < 1/256, the inverse-square term
    // becomes unreliably large or P(t) is near zero.  The paper says
    // to use p_zap = p_sred directly in this range.
    double pZap;
    if (m_pHit >= (1.0 / 256.0))
    {
        // Normal regime: P(t) >= 1/256 → fewer than ~256 flows
        double invScale = 1.0 / (256.0 * m_pHit); // ≈ N / 256
        double invScaleSq = invScale * invScale;  // ≈ N^2 / 65536
        pZap = pSred * std::min(1.0, invScaleSq);
    }
    else
    {
        // Fallback regime: P(t) < 1/256 (including P(t) == 0)
        // Drop flow-count scaling; use base p_sred directly (eq. 4.8).
        pZap = pSred;
    }

    // ---- Step 3: Full SRED hit-multiplier (eq. 5.1) ----
    // p_zap_full = p_zap * (1 + Hit(t) / P(t))
    //
    // Hit(t) is 1 on a hit, 0 on a miss.
    // For a hit:  factor = 1 + 1/P(t) ≈ 1 + N  → penalizes overactive flows.
    // For a miss: factor = 1                    → base probability unchanged.
    double pDrop = pZap;
    if (hit && m_pHit > 0.0)
    {
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

    NS_LOG_DEBUG("SRED prob: pSred=" << pSred << " pZap=" << pZap << " pDrop=" << pDrop << " P(t)="
                                     << m_pHit << " 1/P(t)=" << (m_pHit > 0 ? 1.0 / m_pHit : 0.0)
                                     << " hit=" << hit);

    return pDrop;
}

// -----------------------------------------------------------------------
// Enqueue
// -----------------------------------------------------------------------

bool
SredQueueDisc::DoEnqueue(Ptr<QueueDiscItem> item)
{
    NS_LOG_FUNCTION(this << item);

    uint32_t nQueued = GetInternalQueue(0)->GetNBytes();

    // Compute flow hash from packet
    uint32_t flowHash = item->Hash();

    // Zombie list comparison
    bool hit = ZombieListCheck(flowHash);

    // Compute drop probability
    double pDrop = CalculateDropProbability(nQueued, hit);

    NS_LOG_DEBUG("\t bytesInQueue " << GetInternalQueue(0)->GetNBytes() << "\t packetsInQueue "
                                    << GetInternalQueue(0)->GetNPackets() << "\t pHit " << m_pHit
                                    << "\t 1/P(t) " << (m_pHit > 0 ? 1.0 / m_pHit : 0.0)
                                    << "\t pDrop " << pDrop);

    uint32_t dropType = DTYPE_NONE;

    // Force drop if queue is at or above the maximum threshold
    if (nQueued >= static_cast<uint32_t>(m_maxTh))
    {
        NS_LOG_DEBUG("adding DROP FORCED MARK");
        dropType = DTYPE_FORCED;
    }
    else
    {
        // Probabilistic (unforced) drop based on SRED probability
        double u = m_uv->GetValue();
        if (u < pDrop)
        {
            NS_LOG_LOGIC("SRED unforced drop: u=" << u << " < pDrop=" << pDrop);
            dropType = DTYPE_UNFORCED;
        }
    }

    if (dropType == DTYPE_UNFORCED)
    {
        if (!m_useEcn || !Mark(item, UNFORCED_MARK))
        {
            NS_LOG_DEBUG("\t Dropping due to SRED Prob " << pDrop);
            DropBeforeEnqueue(item, UNFORCED_DROP);
            return false;
        }
        NS_LOG_DEBUG("\t Marking due to SRED Prob " << pDrop);
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

// -----------------------------------------------------------------------
// Dequeue
// -----------------------------------------------------------------------

Ptr<QueueDiscItem>
SredQueueDisc::DoDequeue()
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

        NS_LOG_LOGIC("Popped " << item);
        NS_LOG_LOGIC("Number packets " << GetInternalQueue(0)->GetNPackets());
        NS_LOG_LOGIC("Number bytes " << GetInternalQueue(0)->GetNBytes());

        return item;
    }
}

// -----------------------------------------------------------------------
// Peek
// -----------------------------------------------------------------------

Ptr<const QueueDiscItem>
SredQueueDisc::DoPeek()
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

// -----------------------------------------------------------------------
// InitializeParams
// -----------------------------------------------------------------------

void
SredQueueDisc::InitializeParams()
{
    NS_LOG_FUNCTION(this);
    NS_LOG_INFO("Initializing SRED params.");

    // Note: m_ptc (packet-time constant) was removed — it was a RED leftover
    // not used by the SRED drop logic.

    // Initialize zombie list
    m_zombieList.resize(m_zombieListSize);
    m_zombiePopulated = 0;

    // Initialize EWMA hit probability estimate
    m_pHit = 0.0;
    m_sredDropProb = 0.0;

    NS_LOG_DEBUG("\tm_maxTh (B) " << m_maxTh << "; m_pMax " << m_pMax << "; m_zombieListSize "
                                  << m_zombieListSize << "; m_zombieOverwriteProb "
                                  << m_zombieOverwriteProb << "; m_hitProbEwma " << m_hitProbEwma);
}

// -----------------------------------------------------------------------
// CheckConfig
// -----------------------------------------------------------------------

bool
SredQueueDisc::CheckConfig()
{
    NS_LOG_FUNCTION(this);
    if (GetNQueueDiscClasses() > 0)
    {
        NS_LOG_ERROR("SredQueueDisc cannot have classes");
        return false;
    }

    if (GetNPacketFilters() > 0)
    {
        NS_LOG_ERROR("SredQueueDisc cannot have packet filters");
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
        NS_LOG_ERROR("SredQueueDisc needs 1 internal queue");
        return false;
    }

    return true;
}

} // namespace ns3

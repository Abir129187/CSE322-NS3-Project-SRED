#ifndef SRED_QUEUE_DISC_H
#define SRED_QUEUE_DISC_H

#include "queue-disc.h"

#include "ns3/boolean.h"
#include "ns3/data-rate.h"
#include "ns3/nstime.h"
#include "ns3/random-variable-stream.h"

#include <vector>

namespace ns3
{

class TraceContainer;

/**
 * @ingroup traffic-control
 *
 * @brief A SRED (Stabilized RED) packet queue disc
 *
 * SRED stabilizes the queue occupancy independently of the number of
 * active flows by maintaining a "zombie list" — a small, fixed-size
 * cache of recently seen flow identifiers. The zombie list is used to
 * estimate the number of active flows and to penalize unresponsive
 * (high-bandwidth) flows with a higher drop probability.
 */
class SredQueueDisc : public QueueDisc
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    /**
     * @brief SredQueueDisc Constructor
     *
     * Create a SRED queue disc
     */
    SredQueueDisc();

    /**
     * @brief Destructor
     */
    ~SredQueueDisc() override;

    /**
     * @brief Drop types
     */
    enum
    {
        DTYPE_NONE,     //!< Ok, no drop
        DTYPE_FORCED,   //!< A "forced" drop
        DTYPE_UNFORCED, //!< An "unforced" (random) drop
    };

    /**
     * Assign a fixed random variable stream number to the random variables
     * used by this model.  Return the number of streams (possibly zero) that
     * have been assigned.
     *
     * @param stream first stream index to use
     * @return the number of stream indices assigned by this model
     */
    int64_t AssignStreams(int64_t stream);

    /**
     * @brief Get the estimated number of active flows.
     *
     * Returns 1/P(t), where P(t) is the EWMA hit-frequency estimator.
     * Per Proposition 1 of the SRED paper, this is a good estimate of N.
     *
     * @returns estimated number of active flows, or 0 if P(t) == 0
     */
    double GetFlowCountEstimate() const;

    // Reasons for dropping packets
    static constexpr const char* UNFORCED_DROP = "Unforced drop"; //!< Early probability drops
    static constexpr const char* FORCED_DROP = "Forced drop";     //!< Forced drops, queue full
    // Reasons for marking packets
    static constexpr const char* UNFORCED_MARK = "Unforced mark"; //!< Early probability marks
    static constexpr const char* FORCED_MARK = "Forced mark";     //!< Forced marks

  protected:
    /**
     * @brief Dispose of the object
     */
    void DoDispose() override;

  private:
    bool DoEnqueue(Ptr<QueueDiscItem> item) override;
    Ptr<QueueDiscItem> DoDequeue() override;
    Ptr<const QueueDiscItem> DoPeek() override;
    bool CheckConfig() override;

    /**
     * @brief Initialize the queue parameters.
     */
    void InitializeParams() override;

    /**
     * @brief A single entry in the zombie list.
     *
     * Each zombie stores a flow hash, a hit count, and the timestamp
     * of the last hit.
     *
     * @note The timestamp field is populated on every hit/overwrite but is
     *       currently not read.  The paper (Section II) anticipates using it
     *       to increase the overwrite probability for older zombies;
     *       this is reserved for a future enhancement.
     */
    struct ZombieEntry
    {
        uint32_t flowHash; //!< Hash identifying the flow
        uint32_t count;    //!< Number of hits for this zombie
        Time timestamp;    //!< Time of last hit (reserved for future age-weighted overwrite)
        bool valid;        //!< Whether this entry has been populated

        ZombieEntry()
            : flowHash(0),
              count(0),
              timestamp(Seconds(0)),
              valid(false)
        {
        }
    };

    /**
     * @brief Perform the zombie list comparison for an arriving packet.
     *
     * Picks a random zombie and checks for a hit. Updates the zombie
     * list and the EWMA hit-frequency estimator P(t).
     *
     * @param flowHash hash of the arriving packet's flow
     * @return true if a hit occurred (packet matched the selected zombie)
     */
    bool ZombieListCheck(uint32_t flowHash);

    /**
     * @brief Compute the SRED drop probability (Full SRED, eqs. 4.3–5.1).
     *
     * Implements the three-level step function p_sred(q) (eq. 4.3),
     * the inverse-square flow-count scaling p_zap (eq. 4.4), and
     * the Full SRED hit-multiplier (eq. 5.1).
     *
     * @param nQueued current number of packets (or bytes) in the queue
     * @param hit whether the arriving packet caused a zombie-list hit
     * @return the drop probability for this packet
     */
    double CalculateDropProbability(uint32_t nQueued, bool hit);

    // ** Variables supplied by user
    uint32_t m_meanPktSize;       //!< Average packet size
    double m_maxTh;               //!< Buffer size B — forced-drop threshold (packets/bytes)
    double m_pMax;                //!< Maximum early-drop probability in p_sred(q) step function
    uint32_t m_zombieListSize;    //!< Number of entries in the zombie list (M)
    double m_zombieOverwriteProb; //!< Probability of overwriting a zombie on a miss (p)
    double m_hitProbEwma;     //!< EWMA weight alpha = 1/M for hit-frequency estimator (Section III)
    DataRate m_linkBandwidth; //!< Link bandwidth
    Time m_linkDelay;         //!< Link delay
    bool m_useEcn;            //!< True if ECN is used (mark instead of drop)
    bool m_useHardDrop;       //!< True if packets are always dropped when queue is full

    // ** Variables maintained by SRED
    std::vector<ZombieEntry> m_zombieList; //!< The zombie list (flow cache)
    uint32_t m_zombiePopulated;            //!< Number of valid entries in zombie list
    double m_pHit;                         //!< EWMA estimate of hit probability P(t); 1/P(t) ≈ N
    double m_sredDropProb;                 //!< Current SRED drop probability (p_zap or p_zap_full)

    Ptr<UniformRandomVariable> m_uv; //!< Rng stream
};

}; // namespace ns3

#endif // SRED_QUEUE_DISC_H

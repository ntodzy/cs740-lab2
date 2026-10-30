#include "eventlist.h"
#include "logfile.h"
#include "aprx-fairqueue.h"
#include "fairqueue.h"
#include "priorityqueue.h"
#include "stoc-fairqueue.h"
#include "flow-generator.h"
#include "pipe.h"
#include "test.h"

namespace conga {

const uint32_t N_CORE = 12;
const uint32_t N_LEAF = 24;
const uint32_t N_SERVER = 32; // Per leaf.
const uint32_t N_NODES = N_LEAF * N_SERVER;

const uint64_t LEAF_BUFFER = 512000;
const uint64_t CORE_BUFFER = 1024000;
const uint64_t ENDH_BUFFER = 8192000;

const uint64_t LEAF_SPEED = 10000000000ULL; // 10 Gbps.
const uint64_t CORE_SPEED = 40000000000ULL; // 40 Gbps.
const double LINK_DELAY = 0.1;               // Microseconds per direction.

// Every physical link is modeled in both directions. A Queue models the
// transmitting port and a Pipe models propagation delay.
Queue *qServerLeaf[N_LEAF][N_SERVER] = {};
Pipe  *pServerLeaf[N_LEAF][N_SERVER] = {};
Queue *qLeafServer[N_LEAF][N_SERVER] = {};
Pipe  *pLeafServer[N_LEAF][N_SERVER] = {};

Queue *qLeafCore[N_LEAF][N_CORE] = {};
Pipe  *pLeafCore[N_LEAF][N_CORE] = {};
Queue *qCoreLeaf[N_CORE][N_LEAF] = {};
Pipe  *pCoreLeaf[N_CORE][N_LEAF] = {};

void createQueue(const std::string &queueType,
                 Queue *&queue,
                 linkspeed_bps speed,
                 mem_b buffer);
void nameObject(Logged &object, const std::string &name, Logfile &logfile);
void generateRandomRoute(route_t *&fwd,
                         route_t *&rev,
                         uint32_t &src,
                         uint32_t &dst);

} // namespace conga

using namespace std;
using namespace conga;

void
conga_testbed(const ArgList &args, Logfile &logfile)
{
    uint32_t duration = 5;
    uint32_t avgFlowSize = 100000;
    double utilization = 0.1;
    double linkDelay = LINK_DELAY;
    string queueType = "droptail";
    string endHost = "dctcp";
    string flowDist = "uniform";

    parseInt(args, "duration", duration);
    parseInt(args, "flowsize", avgFlowSize);
    parseDouble(args, "utilization", utilization);
    parseDouble(args, "linkdelay", linkDelay);
    parseString(args, "queue", queueType);
    parseString(args, "endhost", endHost);
    parseString(args, "flowdist", flowDist);

    if (duration == 0 || utilization <= 0 || linkDelay < 0) {
        cerr << "duration and utilization must be positive; linkdelay cannot be negative\n";
        exit(1);
    }

    // Connect each of the 24 leaves to all 12 cores. Each core therefore uses
    // all 24 of its 40 Gbps ports, and each leaf uses all 12 uplink ports.
    for (uint32_t leaf = 0; leaf < N_LEAF; leaf++) {
        for (uint32_t core = 0; core < N_CORE; core++) {
            createQueue(queueType, qLeafCore[leaf][core], CORE_SPEED, LEAF_BUFFER);
            nameObject(*qLeafCore[leaf][core],
                       "q-leaf-core-" + to_string(leaf) + "-" + to_string(core),
                       logfile);
            pLeafCore[leaf][core] = new Pipe(timeFromUs(linkDelay));
            nameObject(*pLeafCore[leaf][core],
                       "p-leaf-core-" + to_string(leaf) + "-" + to_string(core),
                       logfile);

            createQueue(queueType, qCoreLeaf[core][leaf], CORE_SPEED, CORE_BUFFER);
            nameObject(*qCoreLeaf[core][leaf],
                       "q-core-leaf-" + to_string(core) + "-" + to_string(leaf),
                       logfile);
            pCoreLeaf[core][leaf] = new Pipe(timeFromUs(linkDelay));
            nameObject(*pCoreLeaf[core][leaf],
                       "p-core-leaf-" + to_string(core) + "-" + to_string(leaf),
                       logfile);
        }
    }

    // Attach each of the 768 servers to exactly one leaf with a 10 Gbps link.
    for (uint32_t leaf = 0; leaf < N_LEAF; leaf++) {
        for (uint32_t server = 0; server < N_SERVER; server++) {
            createQueue("fq", qServerLeaf[leaf][server], LEAF_SPEED, ENDH_BUFFER);
            nameObject(*qServerLeaf[leaf][server],
                       "q-server-leaf-" + to_string(leaf) + "-" + to_string(server),
                       logfile);
            pServerLeaf[leaf][server] = new Pipe(timeFromUs(linkDelay));
            nameObject(*pServerLeaf[leaf][server],
                       "p-server-leaf-" + to_string(leaf) + "-" + to_string(server),
                       logfile);

            createQueue(queueType, qLeafServer[leaf][server], LEAF_SPEED, LEAF_BUFFER);
            nameObject(*qLeafServer[leaf][server],
                       "q-leaf-server-" + to_string(leaf) + "-" + to_string(server),
                       logfile);
            pLeafServer[leaf][server] = new Pipe(timeFromUs(linkDelay));
            nameObject(*pLeafServer[leaf][server],
                       "p-leaf-server-" + to_string(leaf) + "-" + to_string(server),
                       logfile);
        }
    }

    DataSource::EndHost endpoint = DataSource::TCP;
    if (endHost == "dctcp") {
        endpoint = DataSource::DCTCP;
    } else if (endHost == "timely") {
        endpoint = DataSource::TIMELY;
    } else if (endHost == "pp") {
        endpoint = DataSource::PKTPAIR;
    } else if (endHost != "tcp") {
        cerr << "Unknown endhost type: " << endHost << endl;
        exit(1);
    }

    Workloads::FlowDist distribution = Workloads::UNIFORM;
    if (flowDist == "pareto") {
        distribution = Workloads::PARETO;
    } else if (flowDist == "enterprise") {
        distribution = Workloads::ENTERPRISE;
    } else if (flowDist == "datamining") {
        distribution = Workloads::DATAMINING;
    } else if (flowDist != "uniform") {
        cerr << "Unknown flow distribution: " << flowDist << endl;
        exit(1);
    }

    // Offered load is relative to the aggregate 10 Gbps server-facing
    // capacity. The shared server-to-leaf queues enforce each host's link rate.
    linkspeed_bps flowRate = llround(utilization * LEAF_SPEED * N_NODES);
    FlowGenerator *flowGenerator = new FlowGenerator(endpoint,
                                                      generateRandomRoute,
                                                      flowRate,
                                                      avgFlowSize,
                                                      distribution);
    flowGenerator->setTimeLimits(timeFromUs(1), timeFromSec(duration) - 1);
    EventList::Get().setEndtime(timeFromSec(duration));

    cerr << "CONGA topology: " << N_CORE << " cores, " << N_LEAF
         << " leaves, " << N_NODES << " servers" << endl;
}

void
conga::generateRandomRoute(route_t *&fwd,
                           route_t *&rev,
                           uint32_t &src,
                           uint32_t &dst)
{
    src = rand() % N_NODES;
    do {
        dst = rand() % N_NODES;
    } while (dst == src);

    uint32_t srcLeaf = src / N_SERVER;
    uint32_t dstLeaf = dst / N_SERVER;
    uint32_t srcServer = src % N_SERVER;
    uint32_t dstServer = dst % N_SERVER;

    fwd = new route_t();
    rev = new route_t();

    fwd->push_back(qServerLeaf[srcLeaf][srcServer]);
    fwd->push_back(pServerLeaf[srcLeaf][srcServer]);
    rev->push_back(qServerLeaf[dstLeaf][dstServer]);
    rev->push_back(pServerLeaf[dstLeaf][dstServer]);

    // Until CONGA's table-driven choice is implemented, this is the ECMP
    // baseline: one randomly selected equal-cost core is fixed for the flow.
    if (srcLeaf != dstLeaf) {
        uint32_t core = rand() % N_CORE;

        fwd->push_back(qLeafCore[srcLeaf][core]);
        fwd->push_back(pLeafCore[srcLeaf][core]);
        fwd->push_back(qCoreLeaf[core][dstLeaf]);
        fwd->push_back(pCoreLeaf[core][dstLeaf]);

        rev->push_back(qLeafCore[dstLeaf][core]);
        rev->push_back(pLeafCore[dstLeaf][core]);
        rev->push_back(qCoreLeaf[core][srcLeaf]);
        rev->push_back(pCoreLeaf[core][srcLeaf]);
    }

    fwd->push_back(qLeafServer[dstLeaf][dstServer]);
    fwd->push_back(pLeafServer[dstLeaf][dstServer]);
    rev->push_back(qLeafServer[srcLeaf][srcServer]);
    rev->push_back(pLeafServer[srcLeaf][srcServer]);
}

void
conga::createQueue(const string &queueType,
                   Queue *&queue,
                   linkspeed_bps speed,
                   mem_b buffer)
{
    // Per-queue sampling is intentionally disabled here: this topology has
    // 2,112 directional queues and sampling every queue creates very large logs.
    if (queueType == "fq") {
        queue = new FairQueue(speed, buffer, NULL);
    } else if (queueType == "afq") {
        queue = new AprxFairQueue(speed, buffer, NULL);
    } else if (queueType == "pq") {
        queue = new PriorityQueue(speed, buffer, NULL);
    } else if (queueType == "sfq") {
        queue = new StocFairQueue(speed, buffer, NULL);
    } else if (queueType == "droptail") {
        queue = new Queue(speed, buffer, NULL);
    } else {
        cerr << "Unknown queue type: " << queueType << endl;
        exit(1);
    }
}

void
conga::nameObject(Logged &object, const string &name, Logfile &logfile)
{
    object.setName(name);
    logfile.writeName(object);
}

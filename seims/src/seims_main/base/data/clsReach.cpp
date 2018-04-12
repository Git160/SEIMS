#include "clsReach.h"

using namespace std;

const int REACH_PARAM_NUM = 41; /// Numerical parameters
const char* REACH_PARAM_NAME[] = {
    REACH_SUBBASIN, REACH_NUMCELLS, // 0-1
    REACH_DOWNSTREAM, REACH_UPDOWN_ORDER, REACH_DOWNUP_ORDER, // 2-4
    REACH_WIDTH, REACH_SIDESLP, REACH_LENGTH, REACH_DEPTH, // 5-8
    REACH_V0, REACH_AREA, REACH_MANNING, REACH_SLOPE, // 9-12
    REACH_KBANK, REACH_KBED, REACH_COVER, REACH_EROD, // 13-16
    REACH_BC1, REACH_BC2, REACH_BC3, REACH_BC4, // 17-20
    REACH_RS1, REACH_RS2, REACH_RS3, REACH_RS4, REACH_RS5, // 21-25
    REACH_RK1, REACH_RK2, REACH_RK3, REACH_RK4, // 26-29
    REACH_DISOX, REACH_BOD, REACH_ALGAE, // 30-32
    REACH_ORGN, REACH_NH4, REACH_NO2, REACH_NO3, // 33-36
    REACH_ORGP, REACH_SOLP, REACH_GWNO3, REACH_GWSOLP // 37-40
};
const int REACH_GROUP_METHOD_NUM = 2; /// Group methods
const char* REACH_GROUP_NAME[] = {REACH_KMETIS, REACH_PMETIS};

clsReach::clsReach(const bson_t*& bsonTable) {
    bson_iter_t iterator;
    // read numerical parameters
    for (int i = 0; i < REACH_PARAM_NUM; i++) {
        float tmp_param;
        if (bson_iter_init_find(&iterator, bsonTable, REACH_PARAM_NAME[i])) {
            if (GetNumericFromBsonIterator(&iterator, tmp_param)) {
                if (REACH_PARAM_NAME[i] == REACH_BOD && tmp_param < 1.e-6f) tmp_param = 1.e-6f;
                m_paramMap.insert(make_pair(REACH_PARAM_NAME[i], tmp_param));
            }
        }
    }
    // read group informations, DO NOT THROW EXCEPTION!
    if (!bson_iter_init_find(&iterator, bsonTable, REACH_GROUP)) {
        cout << "No GROUP field found!" << endl;
        return;
    }
    m_groupNumber = SplitStringForInt(GetStringFromBsonIterator(&iterator), ',');
    for (int i = 0; i < REACH_GROUP_METHOD_NUM; i++) {
        if (!bson_iter_init_find(&iterator, bsonTable, REACH_GROUP_NAME[i])) continue;
        vector<int> gIdx = SplitStringForInt(GetStringFromBsonIterator(&iterator), ',');
        if (m_groupNumber.size() != gIdx.size()) {
            cout << "The size of group method " << REACH_GROUP_NAME[i] << " is not equal to GROUP! " << endl;
            continue;
        }
        map<int, int> group_index;
        for (int j = 0; j < m_groupNumber.size(); j++) {
            group_index.insert(make_pair(m_groupNumber[j], gIdx[j]));
        }
        if (group_index.empty()) continue;
        m_groupIndex.insert(make_pair(REACH_GROUP_NAME[i], group_index));
    }
}

float clsReach::Get(const string& key) {
    auto it = m_paramMap.find(key);
    if (it != m_paramMap.end()) {
        return it->second;
    }
    return NODATA_VALUE;
}

int clsReach::GetGroupIndex(const string& method, int size) {
    if (m_groupIndex.empty()) return -1;
    if (m_groupIndex.find(method) == m_groupIndex.end()) return -1;
    map<int, int> tmp = m_groupIndex.at(method);
    if (tmp.find(size) == tmp.end()) return -1;
    return tmp.at(size);
}

void clsReach::Set(const string& key, float value) {
    auto it = m_paramMap.find(key);
    if (it != m_paramMap.end()) {
        m_paramMap.at(key) = value;
    } else { m_paramMap.insert(make_pair(key, NODATA_VALUE)); }
}

clsReaches::clsReaches(MongoClient* conn, string& dbName, string collectionName,
                       LayeringMethod mtd /* = UP_DOWN */) {
    bson_t* b = bson_new();
    bson_t* child1 = bson_new();
    bson_t* child2 = bson_new();
    BSON_APPEND_DOCUMENT_BEGIN(b, "$query", child1); /// query all records
    bson_append_document_end(b, child1);
    BSON_APPEND_DOCUMENT_BEGIN(b, "$orderby", child2); /// and order by subbasin ID
    BSON_APPEND_INT32(child2, REACH_SUBBASIN, 1);
    bson_append_document_end(b, child2);
    bson_destroy(child1);
    bson_destroy(child2);

    unique_ptr<MongoCollection> collection(new MongoCollection(conn->getCollection(dbName, collectionName)));
    this->m_reachNum = collection->QueryRecordsCount();
    if (this->m_reachNum < 0) {
        bson_destroy(b);
        throw ModelException("clsReaches", "ReadAllReachInfo",
                             "Failed to get document number of collection: " + collectionName + ".\n");
    }
    this->m_reachUpStream.resize(this->m_reachNum + 1);

    mongoc_cursor_t* cursor = collection->ExecuteQuery(b);
    const bson_t* bsonTable;
    while (mongoc_cursor_more(cursor) && mongoc_cursor_next(cursor, &bsonTable)) {
        clsReach* curReach = new clsReach(bsonTable);
        int subID = int(curReach->Get(REACH_SUBBASIN));
        this->m_reachesMap.insert(make_pair(subID, curReach));
    }
    // In SEIMS, reach ID is the same as Index of array and vector.
    for (int i = 1; i <= m_reachNum; i++) {
        int downStreamId = int(m_reachesMap.at(i)->Get(REACH_DOWNSTREAM));
        m_reachDownStream.insert(make_pair(i, downStreamId));
        if (downStreamId <= 0 || downStreamId > m_reachNum) continue;
        m_reachUpStream[downStreamId].push_back(i);
    }
    // Build layers of reaches according to layering method
    m_reachLayers.clear();
    for (int i = 1; i <= m_reachNum; i++) {
        int order = int(m_reachesMap.at(i)->Get(REACH_UPDOWN_ORDER));
        if (mtd == DOWN_UP) order = int(m_reachesMap.at(i)->Get(REACH_DOWNUP_ORDER));
        if (m_reachLayers.find(order) == m_reachLayers.end()) {
            m_reachLayers.insert(make_pair(order, vector<int>()));
        }
        m_reachLayers.at(order).push_back(i);
    }

    bson_destroy(b);
    mongoc_cursor_destroy(cursor);
}

clsReaches::~clsReaches() {
    StatusMessage("Release clsReach...");
    if (!m_reachesMap.empty()) {
        for (auto iter = m_reachesMap.begin(); iter != m_reachesMap.end();) {
            if (nullptr != iter->second) {
                delete iter->second;
                iter->second = nullptr;
            }
            m_reachesMap.erase(iter++);
        }
        m_reachesMap.clear();
    }
    if (!m_reachesPropMap.empty()) {
        for (auto iter = m_reachesPropMap.begin(); iter != m_reachesPropMap.end();) {
            if (nullptr != iter->second) {
                Release1DArray(iter->second);
            }
            m_reachesPropMap.erase(iter++);
        }
        m_reachesPropMap.clear();
    }
}

void clsReaches::GetReachesSingleProperty(string key, float** data) {
    auto iter = m_reachesPropMap.find(key);
    if (iter == m_reachesPropMap.end()) {
        float* values = nullptr;
        Initialize1DArray(m_reachNum + 1, values, 0.f);
        values[0] = m_reachNum;
        for (auto it = m_reachesMap.begin(); it != m_reachesMap.end(); ++it) {
            values[it->first] = it->second->Get(key);
        }
        *data = values;
    } else {
        *data = iter->second;
    }
}

void clsReaches::Update(const map<string, ParamInfo *>& caliparams_map) {
    for (int i = 0; i < REACH_PARAM_NUM; i++) {
        auto it = caliparams_map.find(REACH_PARAM_NAME[i]);
        if (it != caliparams_map.end()) {
            ParamInfo* tmpParam = it->second;
            if ((StringMatch(tmpParam->Change, PARAM_CHANGE_RC) && FloatEqual(tmpParam->Impact, 1.f)) ||
                (StringMatch(tmpParam->Change, PARAM_CHANGE_AC) && FloatEqual(tmpParam->Impact, 0.f)) ||
                (StringMatch(tmpParam->Change, PARAM_CHANGE_VC) && FloatEqual(tmpParam->Impact, NODATA_VALUE)) ||
                (StringMatch(tmpParam->Change, PARAM_CHANGE_NC))) {
                continue;
            }
            for (auto it2 = m_reachesMap.begin(); it2 != m_reachesMap.end(); ++it2) {
                float pre_value = it2->second->Get(REACH_PARAM_NAME[i]);
                if (FloatEqual(pre_value, NODATA_VALUE)) continue;
                float new_value = it->second->GetAdjustedValue(pre_value);
                it2->second->Set(REACH_PARAM_NAME[i], new_value);
            }
        }
    }
}

//
// Created by szu on 2021/11/17.
//

#include <cmath>
#include "dataflow/event.h"

namespace {

int TransactionCount(int bytes, int block_size) {
    if (block_size <= 0) {
        throw std::invalid_argument("event block size must be positive");
    }
    if (bytes <= 0) {
        throw std::invalid_argument("event byte count must be positive");
    }
    return static_cast<int>(std::ceil(static_cast<double>(bytes) / block_size));
}

}  // namespace

// todo add capacity limitation for event queue
Event::Event(uint64_t &addr,
             int bytes,
             EventDirType dir_type,
             EventType event_type,
             EventDataType data_type):
        address(addr), bytes(bytes),
        dir_type(dir_type),
        event_type(event_type),
        data_type(data_type),
        num_trans(TransactionCount(bytes, sz_block)),
        trans_cnt(0),
        confirm_cnt(0) {
    static std::unordered_map<EventType, std::string> type_map = {{EventType::WRITE,            "WRITE"},
                                                                  {EventType::READ,             "READ"},
                                                                  {EventType::READ_AGGR,        "READ_AGGR"}};
    static std::unordered_map<EventDirType, std::string> dir_map = {{EventDirType::EDRAM,       "EDRAM"},
                                                                    {EventDirType::DRAM,        "DRAM"},
                                                                    {EventDirType::NONE,        "NONE"}};
    static std::unordered_map<EventDataType, std::string> data_map = {{EventDataType::EDGE,     "EDGE"},
                                                                      {EventDataType::WEIGHT,         "WEIGHT"},
                                                                      {EventDataType::INPUT_FEATURE,  "INPUT_FEATURE"},
                                                                      {EventDataType::AGGR_FEATURE,    "AGGR_FEATURE"},
                                                                      {EventDataType::OUTPUT_FEATURE, "OUTPUT_FEATURE"},
                                                                      {EventDataType::NONE,           "NONE"}};

//    std::cout << "Event{"
//              << "req:" << address
//              << ",bytes:" << bytes
//              << ",dir:" << dir_map[dir_type]
//              << ",type:" << type_map[event_type]
//              << ",data:" << data_map[data_type]
//              << "}" << std::endl;


}



int Event::sz_block = 0;

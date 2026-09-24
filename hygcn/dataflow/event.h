//
// Created by szu on 2021/11/17.
//

#ifndef DRAMSIM3_EVENT_H
#define DRAMSIM3_EVENT_H

#include "common.h"
#include <unordered_map>
#include <memory>
#include <cassert>
#include <limits>
#include <stdexcept>

enum class EventDirType {
    EDRAM, // aggr
    DRAM,
    NONE
};

enum class EventType {
    WRITE,
    READ,
    READ_AGGR
};

enum class EventDataType {
    EDGE,
    WEIGHT,
    INPUT_FEATURE,
    AGGR_FEATURE,
    OUTPUT_FEATURE,
    NONE
};

class Event {
public:

    // todo add capacity limitation for event queue
    Event(uint64_t &addr,
          int bytes,
          EventDirType dir_type,
          EventType event_type,
          EventDataType data_type);

    bool AllTransConfirmed() const {
        return confirm_cnt == num_trans;
    }

    bool AllTransIssued() const {
        return trans_cnt == num_trans;
    }

    void ConfirmTrans() {
        if (confirm_cnt >= num_trans || confirm_cnt >= trans_cnt) {
            throw std::logic_error("event transaction confirmation overflow");
        }
        confirm_cnt++;
    }

    void IssueTrans() {
        if (trans_cnt >= num_trans) {
            throw std::logic_error("event transaction issue overflow");
        }
        trans_cnt++;
    }

    void ValidateAddressRange(uint64_t capacity) const {
        if (address >= capacity || static_cast<uint64_t>(bytes) > capacity - address) {
            throw std::out_of_range("event address range exceeds capacity");
        }
    }

    void ValidateComplete() const {
        if (!AllTransIssued() || !AllTransConfirmed()) {
            throw std::logic_error("event has unfinished transactions");
        }
    }

    const uint64_t address;
    const int bytes;
    const EventDirType dir_type;
    const EventType event_type;
    const EventDataType data_type;
    static int sz_block;

    int trans_cnt;
    int confirm_cnt;

private:
    const int num_trans;

};

using EventPtr = std::shared_ptr<Event>;
#endif //DRAMSIM3_EVENT_H

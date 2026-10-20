#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace mws::enginepass {
struct AlphaData { void* object{}; void* callback{}; float distance{}; };
struct AlphaNode { AlphaData data; AlphaNode *prev{}, *next{}; };
struct AlphaList { AlphaNode head, tail, freeHead, freeTail; AlphaNode* allocation{}; };
static_assert(sizeof(void*) != 4 || sizeof(AlphaList) == 0x54);
// Preserve a continuously sorted game-owned linked list. A contiguous index
// replaces pointer-chasing search, without delaying sorting past observers.
class AlphaIndex {
    std::array<AlphaNode*,512> nodes_{};
    AlphaList* list_{};
    AlphaNode *head_{}, *tail_{}, *free_{};
    unsigned count_{};
    void snapshot(AlphaList& l) { head_=l.head.next; tail_=l.tail.prev; free_=l.freeHead.next; }
public:
    uint64_t comparisons{};
    void invalidate() { list_=nullptr; count_=0; }
    bool insert(AlphaList& l, const AlphaData& data, AlphaNode*& result) {
        if (!std::isfinite(data.distance)) { invalidate(); return false; }
        if (list_!=&l || head_!=l.head.next || tail_!=l.tail.prev || free_!=l.freeHead.next) {
            list_=&l; count_=0;
            for(auto* n=l.head.next; n!=&l.tail; n=n->next) {
                if (!n || count_==nodes_.size() || !std::isfinite(n->data.distance) ||
                    (count_ && nodes_[count_-1]->data.distance > n->data.distance)) { invalidate(); return false; }
                nodes_[count_++]=n;
            }
            snapshot(l);
        }
        if (l.freeHead.next==&l.freeTail) { result=nullptr; return true; }
        if (count_==nodes_.size()) { invalidate(); return false; }
        unsigned lo=0,hi=count_;
        while(lo<hi) { const auto mid=(lo+hi)/2; ++comparisons;
            if(nodes_[mid]->data.distance < data.distance) lo=mid+1; else hi=mid;
        }
        auto* successor=lo<count_ ? nodes_[lo] : &l.tail;
        auto* n=l.freeHead.next;
        n->prev->next=n->next; n->next->prev=n->prev;
        n->data=data; n->prev=successor->prev; n->next=successor;
        successor->prev->next=n; successor->prev=n;
        std::move_backward(nodes_.begin()+lo,nodes_.begin()+count_,nodes_.begin()+count_+1);
        nodes_[lo]=n; ++count_; snapshot(l); result=n;
        return true;
    }
};

struct ShadowKey { uintptr_t texture{}; uint8_t type{}; };
class ShadowGroups {
    struct Bucket { bool used{}; ShadowKey key{}; unsigned head{}; };
    std::array<Bucket,128> buckets_{};
    std::array<unsigned,48> next_{};
    unsigned count_{};
public:
    bool build(const ShadowKey* keys,unsigned count) {
        if(count>next_.size()) return false;
        buckets_={}; count_=count;
        for(unsigned i=count;i-->0;) {
            auto slot=((keys[i].texture>>4)^keys[i].type*2654435761u)&127;
            while(buckets_[slot].used && (buckets_[slot].key.texture!=keys[i].texture || buckets_[slot].key.type!=keys[i].type)) slot=(slot+1)&127;
            auto& b=buckets_[slot]; next_[i]=b.used ? b.head : count;
            b={true,keys[i],i};
        }
        return true;
    }
    unsigned next(unsigned current) const { return current<count_ ? next_[current] : count_; }
};

} // namespace mws::enginepass

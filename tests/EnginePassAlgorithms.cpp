#include "mws/EnginePassAlgorithms.hpp"
#include <cstdio>
#include <random>
#include <vector>
using namespace mws::enginepass;
#define CHECK(x) do {if(!(x)){std::printf("Failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main() {
    std::mt19937 rng(74521);
    AlphaList list; std::array<AlphaNode,200> pool;
    auto reset=[&] {
        list={};list.head.next=&list.tail;list.tail.prev=&list.head;
        list.freeHead.next=pool.data();list.freeTail.prev=&pool.back();
        for(unsigned i=0;i<pool.size();++i){pool[i]={};pool[i].prev=i?&pool[i-1]:&list.freeHead;pool[i].next=i+1<pool.size()?&pool[i+1]:&list.freeTail;}
    };
    AlphaIndex index;
    for(unsigned trial=0;trial<40;++trial) {
        reset();std::vector<std::pair<float,uintptr_t>> expected;
        for(unsigned i=0;i<pool.size();++i) {
            const float distance=float(rng()%37);
            const uintptr_t id=i+1;
            AlphaNode* result{};
            CHECK(index.insert(list,{reinterpret_cast<void*>(id),nullptr,distance},result));
            CHECK(result==&pool[i]);
            const auto place=std::lower_bound(expected.begin(),expected.end(),distance,[](const auto& entry,float d){return entry.first<d;});
            expected.insert(place,{distance,id});
            unsigned n{};AlphaNode* previous=&list.head;
            for(auto* node=list.head.next;node!=&list.tail;node=node->next) {
                CHECK(n<expected.size() && node->prev==previous);
                CHECK(node->data.distance==expected[n].first && reinterpret_cast<uintptr_t>(node->data.object)==expected[n].second);
                previous=node;++n;
            }
            CHECK(n==expected.size() && list.tail.prev==previous);
        }
        AlphaNode* full=reinterpret_cast<AlphaNode*>(1);
        CHECK(index.insert(list,{nullptr,nullptr,3},full) && full==nullptr);
        // External removal changes the free pool, forcing index reconstruction.
        auto* removed=list.head.next->next;
        removed->prev->next=removed->next;removed->next->prev=removed->prev;
        removed->prev=&list.freeHead;removed->next=&list.freeTail;
        list.freeHead.next=removed;list.freeTail.prev=removed;
        CHECK(index.insert(list,{nullptr,nullptr,19},full) && full==removed);
    }
    AlphaNode* result{};
    CHECK(!index.insert(list,{nullptr,nullptr,std::numeric_limits<float>::quiet_NaN()},result));
    ShadowGroups groups;std::array<ShadowKey,48> keys;
    for(unsigned trial=0;trial<1000;++trial) {
        const unsigned count=rng()%49;
        for(unsigned i=0;i<count;++i) keys[i]={uintptr_t(rng()%13)*16,uint8_t(rng()%5)};
        CHECK(groups.build(keys.data(),count));
        for(unsigned i=0;i<count;++i) {
            unsigned next=i+1;
            while(next<count && (keys[next].texture!=keys[i].texture || keys[next].type!=keys[i].type)) ++next;
            CHECK(groups.next(i)==next);
        }
    }
    CHECK(!groups.build(keys.data(),49));
    std::printf("8,000 alpha insertions, clear/removal/tie/full-pool/NaN cases, and 1,000 shadow groups passed.\n");
}

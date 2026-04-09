#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "shared_work_queue.h"
#include "test_support.h"

namespace {

WorkItem MakeItem(WorkItemType type, std::uint64_t job_id) {
    WorkItem item;
    item.type = type;
    item.job_id = job_id;
    return item;
}

int TestSlotReleasedOnlyAfterDualAck() {
    SharedWorkQueue queue(1);

    TEST_ASSERT(queue.publish(MakeItem(WorkItemType::BeginJob, 1), std::chrono::milliseconds(10)),
                "first publish should succeed");

    SharedWorkQueue::Lease save_lease;
    TEST_ASSERT(queue.pop(SharedWorkConsumer::Save, &save_lease),
                "save consumer should receive the item");
    queue.ack(save_lease, true);

    TEST_ASSERT(!queue.publish(MakeItem(WorkItemType::EndJob, 2), std::chrono::milliseconds(10)),
                "slot must stay occupied until the stream consumer also acks");

    SharedWorkQueue::Lease stream_lease;
    TEST_ASSERT(queue.pop(SharedWorkConsumer::Stream, &stream_lease),
                "stream consumer should receive the same item");
    queue.ack(stream_lease, true);

    TEST_ASSERT(queue.publish(MakeItem(WorkItemType::EndJob, 2), std::chrono::milliseconds(10)),
                "slot should be released after both consumers ack");
    queue.close();
    return 0;
}

int TestConsumersKeepIndependentFifoOrder() {
    SharedWorkQueue queue(2);
    TEST_ASSERT(queue.publish(MakeItem(WorkItemType::BeginJob, 10), std::chrono::milliseconds(10)),
                "first publish should succeed");
    TEST_ASSERT(queue.publish(MakeItem(WorkItemType::LightChunk, 11), std::chrono::milliseconds(10)),
                "second publish should succeed");

    SharedWorkQueue::Lease save0;
    SharedWorkQueue::Lease save1;
    SharedWorkQueue::Lease stream0;
    SharedWorkQueue::Lease stream1;
    TEST_ASSERT(queue.pop(SharedWorkConsumer::Save, &save0), "save consumer should pop first item");
    TEST_ASSERT(queue.pop(SharedWorkConsumer::Save, &save1), "save consumer should pop second item");
    TEST_ASSERT(queue.pop(SharedWorkConsumer::Stream, &stream0), "stream consumer should pop first item");
    TEST_ASSERT(queue.pop(SharedWorkConsumer::Stream, &stream1), "stream consumer should pop second item");

    TEST_ASSERT(save0.item && save0.item->job_id == 10, "save FIFO must preserve first job");
    TEST_ASSERT(save1.item && save1.item->job_id == 11, "save FIFO must preserve second job");
    TEST_ASSERT(stream0.item && stream0.item->job_id == 10, "stream FIFO must preserve first job");
    TEST_ASSERT(stream1.item && stream1.item->job_id == 11, "stream FIFO must preserve second job");

    queue.ack(save0, true);
    queue.ack(stream0, true);
    queue.ack(save1, true);
    queue.ack(stream1, true);
    queue.close();
    return 0;
}

int TestFailureStopsQueueForBothSides() {
    SharedWorkQueue queue(1);
    TEST_ASSERT(queue.publish(MakeItem(WorkItemType::BeginJob, 99), std::chrono::milliseconds(10)),
                "publish should succeed before failure");

    SharedWorkQueue::Lease stream_lease;
    TEST_ASSERT(queue.pop(SharedWorkConsumer::Stream, &stream_lease),
                "stream consumer should receive the item");
    queue.ack(stream_lease, false, "stream failed");

    SharedWorkQueue::Lease save_lease;
    TEST_ASSERT(!queue.pop(SharedWorkConsumer::Save, &save_lease),
                "save consumer should stop once the queue is failed");
    TEST_ASSERT(!queue.publish(MakeItem(WorkItemType::EndJob, 100), std::chrono::milliseconds(10)),
                "new publish should fail after queue failure");
    TEST_ASSERT(queue.failed(), "queue should record failed state");
    TEST_ASSERT(StringContains(queue.failure_reason(), "stream failed"),
                "failure reason should be preserved");
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_shared_work_queue", {
        {"SlotReleasedOnlyAfterDualAck", TestSlotReleasedOnlyAfterDualAck},
        {"ConsumersKeepIndependentFifoOrder", TestConsumersKeepIndependentFifoOrder},
        {"FailureStopsQueueForBothSides", TestFailureStopsQueueForBothSides},
    });
}

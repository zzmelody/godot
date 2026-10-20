#ifndef VIRTUAL_GEOMETRY_DEMAND_QUEUE_H
#define VIRTUAL_GEOMETRY_DEMAND_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <set>

namespace VirtualGeometry {

// One entry per queued page. Handles remain valid until pop or raise; raising
// returns the replacement handle and preserves the original FIFO position.
template <class Key>
class PageDemandQueue {
public:
	struct Demand {
		Key key;
		float priority;
		uint64_t order;
		bool operator<(const Demand &p_other) const {
			return priority == p_other.priority ? order < p_other.order : priority > p_other.priority;
		}
	};
	using Handle = typename std::set<Demand>::const_iterator;

private:
	std::set<Demand> queued;
	uint64_t order = 0;

public:
	PageDemandQueue() = default;
	PageDemandQueue(const PageDemandQueue &) = delete;
	PageDemandQueue &operator=(const PageDemandQueue &) = delete;

	Handle push(const Key &p_key, float p_priority) {
		return queued.insert({ p_key, p_priority, ++order }).first;
	}
	Handle raise(Handle p_handle, float p_priority) {
		if (p_priority <= p_handle->priority) {
			return p_handle;
		}
		Demand replacement = *p_handle;
		replacement.priority = p_priority;
		const Handle result = queued.insert(replacement).first;
		queued.erase(p_handle);
		return result;
	}
	bool empty() const { return queued.empty(); }
	std::size_t size() const { return queued.size(); }
	const Demand &top() const { return *queued.begin(); }
	void pop() { queued.erase(queued.begin()); }
};

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_DEMAND_QUEUE_H

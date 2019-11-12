#include <queue>
#include <stack>
#include <string>
#include <tuple>

#include <iostream>
#include "absl/strings/str_join.h"
#include "src/carnot/plan/dag.h"

namespace pl {
namespace carnot {
namespace plan {

using std::begin;
using std::end;
using std::vector;

void DAG::Init(const planpb::DAG& dag) {
  for (const auto& node : dag.nodes()) {
    AddNode(node.id());
    for (int64_t child : node.sorted_children()) {
      forward_edges_by_node_[node.id()].push_back(child);
    }
    for (int64_t parent : node.sorted_parents()) {
      reverse_edges_by_node_[node.id()].push_back(parent);
    }
  }
}

void DAG::ToProto(planpb::DAG* dag) const {
  for (int64_t i : TopologicalSort()) {
    planpb::DAG_DAGNode* node = dag->add_nodes();
    node->set_id(i);
    auto reverse_it = reverse_edges_by_node_.find(i);
    DCHECK(reverse_it != reverse_edges_by_node_.end());
    for (int64_t parent : reverse_it->second) {
      node->add_sorted_parents(parent);
    }

    auto forward_it = forward_edges_by_node_.find(i);
    DCHECK(forward_it != forward_edges_by_node_.end());
    for (int64_t child : forward_it->second) {
      node->add_sorted_children(child);
    }
  }
}

void DAG::ToProto(planpb::DAG* dag, const absl::flat_hash_set<int64_t>& ignore_ids) const {
  for (int64_t i : TopologicalSort()) {
    if (ignore_ids.contains(i)) {
      continue;
    }
    planpb::DAG_DAGNode* node = dag->add_nodes();
    node->set_id(i);
    auto reverse_it = reverse_edges_by_node_.find(i);
    DCHECK(reverse_it != reverse_edges_by_node_.end());
    for (int64_t parent : reverse_it->second) {
      if (!ignore_ids.contains(parent)) {
        node->add_sorted_parents(parent);
      }
    }

    auto forward_it = forward_edges_by_node_.find(i);
    DCHECK(forward_it != forward_edges_by_node_.end());
    for (int64_t child : forward_it->second) {
      if (!ignore_ids.contains(child)) {
        node->add_sorted_children(child);
      }
    }
  }
}

void DAG::AddNode(int64_t node) {
  DCHECK(!HasNode(node)) << absl::Substitute("Node: $0 already exists", node);
  nodes_.insert(node);

  forward_edges_by_node_[node] = {};
  reverse_edges_by_node_[node] = {};
}

bool DAG::HasNode(int64_t node) const { return nodes_.find(node) != end(nodes_); }

void DAG::DeleteNode(int64_t node) {
  if (!HasNode(node)) {
    LOG(WARNING) << absl::StrCat("Node does not exist: ", node);
  }

  DeleteParentEdges(node);
  DeleteDependentEdges(node);

  nodes_.erase(node);
}

void DAG::AddEdge(int64_t from_node, int64_t to_node) {
  CHECK(HasNode(from_node)) << "from_node does not exist";
  CHECK(HasNode(to_node)) << "to_node does not exist";

  AddForwardEdge(from_node, to_node);
  AddReverseEdge(to_node, from_node);
}

void DAG::AddForwardEdge(int64_t from_node, int64_t to_node) {
  forward_edges_by_node_[from_node].push_back(to_node);
}

void DAG::AddReverseEdge(int64_t to_node, int64_t from_node) {
  reverse_edges_by_node_[to_node].push_back(from_node);
}

void DAG::DeleteParentEdges(int64_t to_node) {
  // Iterate through all of the parents of to_node and delete the edges.
  auto& reverse_edges = reverse_edges_by_node_[to_node];
  auto parent_iter = reverse_edges.begin();
  while (parent_iter != reverse_edges.end()) {
    // Find the forward edge for the specific parent of to_node.
    auto& forward_edges = forward_edges_by_node_[*parent_iter];
    const auto& node = std::find(begin(forward_edges), end(forward_edges), to_node);
    if (node != end(forward_edges)) {
      // Delete parent->to_node edge.
      forward_edges.erase(node);
    }

    // Erase points to the next valid iterator.
    // Delete to_node->parent edge.
    parent_iter = reverse_edges.erase(parent_iter);
  }
}

void DAG::DeleteDependentEdges(int64_t from_node) {
  // Iterate through all of the dependents of from_node and delete the edges.
  auto& forward_edges = forward_edges_by_node_[from_node];
  auto child_iter = forward_edges.begin();
  while (child_iter != forward_edges.end()) {
    // Find the reverse edge for the specific dependent of from_node.
    auto& reverse_edges = reverse_edges_by_node_[*child_iter];
    const auto& node = std::find(begin(reverse_edges), end(reverse_edges), from_node);
    if (node != end(reverse_edges)) {
      // Delete dependent->from_node edge.
      reverse_edges.erase(node);
    }

    // Erase points to the next valid iterator.
    // Delete from_node->dependent edge.
    child_iter = forward_edges.erase(child_iter);
  }
}

void DAG::DeleteEdge(int64_t from_node, int64_t to_node) {
  // If there is a dependency we need to delete both the forward and backwards dependency.
  auto& forward_edges = forward_edges_by_node_[from_node];
  const auto& node = std::find(begin(forward_edges), end(forward_edges), to_node);
  if (node != end(forward_edges)) {
    forward_edges.erase(node);
  }

  auto& reverse_edges = reverse_edges_by_node_[to_node];
  const auto& reverse_node = std::find(begin(reverse_edges), end(reverse_edges), from_node);
  if (reverse_node != end(reverse_edges)) {
    reverse_edges.erase(reverse_node);
  }
}

void DAG::ReplaceChildEdge(int64_t parent_node, int64_t old_child_node, int64_t new_child_node) {
  // If there is a dependency we need to delete both the forward and backwards dependency.
  CHECK(HasNode(parent_node)) << "from_node does not exist";
  CHECK(HasNode(old_child_node)) << "old_child_node does not exist";
  CHECK(HasNode(new_child_node)) << "new_child_node does not exist";
  auto& forward_edges = forward_edges_by_node_[parent_node];

  // Repalce the old_child_node with the new_child_node in the forward edge.
  std::replace(forward_edges.begin(), forward_edges.end(), old_child_node, new_child_node);

  // Remove the old reverse edge (old_child_node, parent_node)
  auto& reverse_edges = reverse_edges_by_node_[old_child_node];
  const auto& reverse_node = std::find(begin(reverse_edges), end(reverse_edges), parent_node);
  if (reverse_node != end(reverse_edges)) {
    reverse_edges.erase(reverse_node);
  }

  // Add the new reverse edge (new_child_node, from_node)
  AddReverseEdge(new_child_node, parent_node);
}

void DAG::ReplaceParentEdge(int64_t child_node, int64_t old_parent_node, int64_t new_parent_node) {
  // If there is a dependency we need to delete both the forward and backwards dependency.
  CHECK(HasNode(child_node)) << "child_node does not exist";
  CHECK(HasNode(old_parent_node)) << "old_parent_node does not exist";
  CHECK(HasNode(new_parent_node)) << "new_parent_node does not exist";
  auto& reverse_edges = reverse_edges_by_node_[child_node];

  // Repalce the old_from_node with the new_from_node.
  std::replace(reverse_edges.begin(), reverse_edges.end(), old_parent_node, new_parent_node);

  // Remove the old forward edge (old_from_node, child_node)
  auto& forward_edges = forward_edges_by_node_[old_parent_node];
  const auto& forward_node = std::find(begin(forward_edges), end(forward_edges), child_node);
  if (forward_node != end(forward_edges)) {
    forward_edges.erase(forward_node);
  }

  // Add the new forward edge (new_from_node, child_node)
  AddForwardEdge(new_parent_node, child_node);
}

bool DAG::HasEdge(int64_t from_node, int64_t to_node) {
  auto& forward_edges = forward_edges_by_node_[from_node];
  const auto& node = std::find(begin(forward_edges), end(forward_edges), to_node);
  return node != end(forward_edges);
}

std::unordered_set<int64_t> DAG::TransitiveDepsFrom(int64_t node) {
  enum VisitStatus { kVisitStarted, kVisitComplete };
  enum NodeColor { kWhite = 0, kGray, kBlack };

  // The visit status related to if we started or completed the visit,
  // the int tracks the node id.
  std::stack<std::tuple<VisitStatus, int64_t>> s;
  std::unordered_set<int64_t> dep_list;
  std::unordered_map<int64_t, NodeColor> colors;

  s.emplace(std::tuple(kVisitStarted, node));

  while (!s.empty()) {
    auto [status, top_node] = s.top();  // NOLINT (cpplint bug)
    s.pop();

    if (status == kVisitComplete) {
      colors[top_node] = kBlack;
    } else {
      colors[top_node] = kGray;
      s.emplace(std::tuple(kVisitComplete, top_node));
      for (auto dep : DependenciesOf(top_node)) {
        CHECK(colors[dep] != kGray) << "Cycle found";
        if (colors[dep] == kWhite) {
          s.emplace(std::tuple(kVisitStarted, dep));
          dep_list.insert(dep);
        }
      }
    }
  }
  return dep_list;
}

std::unordered_set<int64_t> DAG::Orphans() {
  std::unordered_set<int64_t> orphans;
  for (const auto& node : nodes_) {
    if (forward_edges_by_node_[node].empty() && reverse_edges_by_node_[node].empty()) {
      orphans.insert(node);
    }
  }
  return orphans;
}

vector<int64_t> DAG::TopologicalSort() const {
  // Implements Kahn's algorithm:
  // https://en.wikipedia.org/wiki/Topological_sorting#Kahn's_algorithm;
  std::vector<int64_t> ordered;
  ordered.reserve(nodes_.size());
  std::queue<int64_t> q;
  std::unordered_map<int64_t, unsigned int> visited_count;

  // Find nodes that don't have any incoming edges.
  for (auto node : nodes_) {
    if (reverse_edges_by_node_.at(node).empty()) {
      q.push(node);
    }
  }

  CHECK(!q.empty()) << "No nodes without incoming edges, likely a cycle";

  while (!q.empty()) {
    int front_val = q.front();
    q.pop();
    ordered.push_back(front_val);

    for (auto dep : forward_edges_by_node_.at(front_val)) {
      visited_count[dep]++;
      if (visited_count.at(dep) == reverse_edges_by_node_.at(dep).size()) {
        q.push(dep);
      }
    }
  }

  CHECK_EQ(ordered.size(), nodes_.size()) << "Cycle detected in graph";
  return ordered;
}

std::string DAG::DebugString() {
  std::string debug_string;
  for (const auto& node : nodes_) {
    debug_string +=
        absl::Substitute("{$0} : [$1]\n", node, absl::StrJoin(forward_edges_by_node_[node], ", "));
  }
  return debug_string;
}

void DAG::Debug() { LOG(INFO) << "DAG Debug: \n" << DebugString(); }

std::vector<std::unordered_set<int64_t>> DAG::IndependentGraphs() const {
  // The list of source nodes.
  std::vector<int64_t> sources;

  // Find sources: nodes without any incoming edges.
  for (auto node : nodes_) {
    if (reverse_edges_by_node_.at(node).empty()) {
      sources.push_back(node);
    }
  }

  CHECK(!sources.empty()) << "No nodes without incoming edges, likely a cycle";

  // This map keeps track of which set a node belongs to.
  // If key == value, then that is the set_parent of the set with that number.
  // If you merge two sets, then the set_parent of one points to the
  // set_parent of the other.
  std::unordered_map<int64_t, int64_t> set_parents;
  // The map that keeps track of the actual sets.
  std::unordered_map<int64_t, std::unordered_set<int64_t>> out_map;
  for (int64_t source : sources) {
    int64_t current_set_parent = source;
    set_parents[current_set_parent] = current_set_parent;

    // The queue of children to iterate through.
    std::queue<int64_t> q;
    q.push(current_set_parent);

    std::unordered_set<int64_t> current_set({current_set_parent});
    // Iterate through the children.
    while (!q.empty()) {
      auto children = forward_edges_by_node_.at(q.front());
      q.pop();
      for (int64_t child : children) {
        if (set_parents.find(child) != set_parents.end()) {
          // If the child has already been visited, then it already belongs to another set.
          // Point to that set, and merge the existing set.
          int64_t new_set_parent = set_parents[child];
          set_parents[current_set_parent] = new_set_parent;
          current_set_parent = new_set_parent;
          out_map[new_set_parent].merge(current_set);
          current_set = out_map[new_set_parent];

        } else {
          // If the child has been visited, its children have already been visited.
          q.push(child);
        }
        current_set.insert(child);
        set_parents[child] = current_set_parent;
      }
    }
    out_map[current_set_parent] = current_set;
  }

  CHECK_EQ(set_parents.size(), nodes_.size()) << "Cycle detected in graph " << sources.size();

  std::vector<std::unordered_set<int64_t>> out_vec;
  for (const auto& out_map_item : out_map) {
    auto set = out_map_item.second;
    out_vec.push_back(set);
  }

  return out_vec;
}

}  // namespace plan
}  // namespace carnot
}  // namespace pl

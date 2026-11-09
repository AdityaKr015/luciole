#include "browser/directory_tree.h"

#include <algorithm>
#include <set>

struct files::DirectoryTree::Node {
  DirectoryEntry entry;
  DirectorySession session{maxChildren};
  bool started = false;
  struct Identity {
    EntryId id;
    std::optional<FileIdentity> identity;
  };
  std::map<std::string, Identity> ids;
  explicit Node(DirectoryEntry value) : entry(std::move(value)) {}
};
files::DirectoryTree::DirectoryTree(DirectoryModel& model, bool foldersOnly)
    : m_model(model), m_foldersOnly(foldersOnly) {}
files::DirectoryTree::~DirectoryTree() { clear(); }
void files::DirectoryTree::clear() {
  for (auto& [uri, node] : m_nodes) {
    node->session.setChangedCallback({});
  }
  m_nodes.clear();
  m_rejected.reset();
}
void files::DirectoryTree::setRoot(DirectorySnapshot snapshot) {
  if (m_root.location != snapshot.location) {
    clear();
  }
  m_root = std::move(snapshot);
  publish();
}
std::size_t files::DirectoryTree::loading() const {
  return std::ranges::count_if(m_nodes, [](const auto& pair) {
    return pair.second->started && pair.second->session.snapshot().status == DirectoryStatus::Loading;
  });
}
files::TreeRowInfo files::DirectoryTree::row(EntryId id) const {
  TreeRowInfo result{.depth = m_model.treeDepth(id), .enabled = true, .status = {}};
  const auto* entry = m_model.entry(id);
  result.expandable = entry && entry->type == FileType::Directory && !entry->symlink && !entry->location.isTrash();
  if (!entry) {
    return result;
  }
  if (const auto found = m_nodes.find(id); found != m_nodes.end()) {
    result.expanded = true;
    const auto& node = *found->second;
    const auto& snapshot = node.session.snapshot();
    result.loading = !node.started || snapshot.status == DirectoryStatus::Loading;
    if (result.loading) {
      result.status = "Loading…";
    } else if (snapshot.error) {
      result.status = snapshot.error->message;
    } else if (snapshot.status == DirectoryStatus::Ready && snapshot.entries.empty()) {
      result.status = "Empty";
    }
  }
  if (m_rejected && m_rejected->first == id) {
    result.status = m_rejected->second;
  }
  return result;
}
bool files::DirectoryTree::cycle(const DirectoryEntry& entry) const {
  auto parent = m_model.treeParent(entry.id);
  std::set<EntryId> visited;
  while (parent) {
    if (!visited.insert(*parent).second) {
      return true;
    }
    const auto* ancestor = m_model.entry(*parent);
    if (!ancestor) {
      break;
    }
    if (ancestor->location == entry.location || (entry.identity && ancestor->identity == entry.identity)) {
      return true;
    }
    parent = m_model.treeParent(*parent);
  }
  return false;
}
bool files::DirectoryTree::expand(EntryId id) {
  const auto info = row(id);
  if (!info.expandable || info.expanded) {
    return false;
  }
  const auto entry = *m_model.entry(id);
  if (info.depth >= maxDepth || m_nodes.size() >= maxBranches || cycle(entry)) {
    m_rejected = {id, cycle(entry) ? "Folder cycle blocked" : "Tree limit reached. Open this folder to continue."};
    if (m_changed) {
      m_changed();
    }
    return false;
  }
  m_rejected.reset();
  auto node = std::make_unique<Node>(entry);
  node->session.setChangedCallback([this] {
    publish();
    pump();
  });
  m_nodes.emplace(id, std::move(node));
  publish();
  pump();
  return true;
}
bool files::DirectoryTree::collapse(EntryId id) {
  const auto* entry = m_model.entry(id);
  if (!entry || !m_nodes.contains(id)) {
    return false;
  }
  auto focused = m_model.selection().focused;
  while (focused && *focused != id) {
    focused = m_model.treeParent(*focused);
  }
  for (auto it = m_nodes.begin(); it != m_nodes.end();) {
    auto parent = std::optional(it->first);
    while (parent && *parent != id) {
      parent = m_model.treeParent(*parent);
    }
    if (parent) {
      it->second->session.setChangedCallback({});
      it = m_nodes.erase(it);
    } else {
      ++it;
    }
  }
  m_rejected.reset();
  publish();
  if (focused) {
    m_model.setFocus(id);
  }
  pump();
  return true;
}
bool files::DirectoryTree::toggle(EntryId id) { return row(id).expanded ? collapse(id) : expand(id); }
void files::DirectoryTree::pump() {
  if (m_pumping) {
    return;
  }
  m_pumping = true;
  while (loading() < maxLoading) {
    const auto found = std::ranges::find_if(m_nodes, [](const auto& pair) { return !pair.second->started; });
    if (found == m_nodes.end()) {
      break;
    }
    auto& node = *found->second;
    node.started = true;
    if (node.session.snapshot().location) {
      node.session.refresh();
    } else {
      node.session.open(node.entry.location);
    }
  }
  m_pumping = false;
}
void files::DirectoryTree::refresh() {
  for (auto& [id, node] : m_nodes) {
    node->session.setChangedCallback({});
    node->session.cancel();
    node->started = false;
    node->session.setChangedCallback([this] {
      publish();
      pump();
    });
  }
  publish();
  pump();
}
void files::DirectoryTree::publish() {
  auto snapshot = m_root;
  std::unordered_map<EntryId, EntryId> parents;
  std::set<EntryId> retained;
  const auto append = [&](this auto&& self, const DirectoryEntry& entry, std::size_t depth) -> void {
    if (depth >= maxDepth + 1) {
      return;
    }
    const auto found = m_nodes.find(entry.id);
    if (found == m_nodes.end()) {
      return;
    }
    auto& node = *found->second;
    if (entry.id != node.entry.id
        || entry.identity != node.entry.identity
        || entry.symlink
        || entry.type != FileType::Directory) {
      return;
    }
    retained.insert(found->first);
    std::map<std::string, Node::Identity> currentIds;
    for (auto child : node.session.snapshot().entries) {
      if (m_foldersOnly && child.type != FileType::Directory) {
        continue;
      }
      const auto prior = node.ids.find(child.location.uri());
      child.id = prior != node.ids.end() && prior->second.identity == child.identity ? prior->second.id : m_nextId++;
      currentIds.emplace(child.location.uri(), Node::Identity{child.id, child.identity});
      parents.emplace(child.id, entry.id);
      snapshot.entries.push_back(child);
      self(child, depth + 1);
    }
    node.ids = std::move(currentIds);
  };
  for (const auto& entry : m_root.entries) {
    append(entry, 0);
  }
  for (auto it = m_nodes.begin(); it != m_nodes.end();) {
    if (!retained.contains(it->first)) {
      it->second->session.setChangedCallback({});
      it = m_nodes.erase(it);
    } else {
      ++it;
    }
  }
  snapshot.generation = m_model.snapshot().generation + 1;
  m_model.applySnapshot(std::move(snapshot), std::move(parents), m_foldersOnly);
  if (m_changed) {
    m_changed();
  }
}

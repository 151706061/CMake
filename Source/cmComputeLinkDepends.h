/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <cstddef>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <cm/optional>

#include "cmGraphAdjacencyList.h"
#include "cmLinkItem.h"
#include "cmListFileCache.h"
#include "cmTargetLinkLibraryType.h"

class cmComputeComponentGraph;
class cmGeneratorTarget;
class cmGlobalGenerator;
class cmMakefile;
class cmSourceFile;
class cmake;

enum class LinkLibrariesStrategy
{
  REORDER_MINIMALLY,
  REORDER_FREELY,
};

/** \class cmComputeLinkDepends
 * \brief Compute link dependencies for targets.
 */
class cmComputeLinkDepends
{
public:
  cmComputeLinkDepends(cmGeneratorTarget const* target,
                       std::string const& config,
                       std::string const& linkLanguage,
                       LinkLibrariesStrategy strategy);
  ~cmComputeLinkDepends();

  cmComputeLinkDepends(cmComputeLinkDepends const&) = delete;
  cmComputeLinkDepends& operator=(cmComputeLinkDepends const&) = delete;

  // Basic information about each link item.
  struct LinkEntry
  {
    LinkEntry() = default;
    LinkEntry(BT<std::string> item, cmGeneratorTarget const* target = nullptr)
      : Item(std::move(item))
      , Target(target)
    {
    }

    static std::string const& DEFAULT;

    enum EntryKind
    {
      Library,
      Object,
      SharedDep,
      Flag,
      // The following member is for the management of items specified
      // through genex $<LINK_GROUP:...>
      Group
    };

    BT<std::string> Item;
    cmGeneratorTarget const* Target = nullptr;
    // The source file representing the external object (used when linking
    // `$<TARGET_OBJECTS>`)
    cmSourceFile const* ObjectSource = nullptr;
    EntryKind Kind = Library;
    // The following member is for the management of items specified
    // through genex $<LINK_LIBRARY:...>
    std::string Feature = std::string(DEFAULT);
  };

  using EntryVector = std::vector<LinkEntry>;
  EntryVector const& Compute();

private:
  // Context information.
  cmGeneratorTarget const* Target = nullptr;
  cmMakefile* Makefile = nullptr;
  cmGlobalGenerator const* GlobalGenerator = nullptr;
  cmake* CMakeInstance;
  std::string Config;
  bool DebugMode = false;
  std::string LinkLanguage;
  cmTargetLinkLibraryType LinkType;
  LinkLibrariesStrategy Strategy;

  EntryVector FinalLinkEntries;
  std::map<std::string, std::string> LinkLibraryOverride;

  std::string const& GetCurrentFeature(
    std::string const& item, std::string const& defaultFeature) const;

  std::pair<std::map<cmLinkItem, size_t>::iterator, bool> AllocateLinkEntry(
    cmLinkItem const& item);
  std::pair<size_t, bool> AddLinkEntry(cmLinkItem const& item,
                                       cm::optional<size_t> groupIndex);
  void AddLinkObject(cmLinkItem const& item);
  void AddVarLinkEntries(cm::optional<size_t> depender_index,
                         char const* value);
  void AddDirectLinkEntries();
  template <typename T>
  void AddLinkEntries(cm::optional<size_t> depender_index,
                      std::vector<T> const& libs);
  void AddLinkObjects(std::vector<cmLinkItem> const& objs);
  cmLinkItem ResolveLinkItem(cm::optional<size_t> depender_index,
                             std::string const& name);

  // One entry for each unique item.
  std::vector<LinkEntry> EntryList;
  std::map<cmLinkItem, size_t> LinkEntryIndex;

  // map storing, for each group, the list of items
  std::map<size_t, std::vector<size_t>> GroupItems;

  // BFS of initial dependencies.
  struct BFSEntry
  {
    size_t Index;
    cm::optional<size_t> GroupIndex;
    char const* LibDepends;
  };
  std::queue<BFSEntry> BFSQueue;
  void FollowLinkEntry(BFSEntry qe);

  // Shared libraries that are included only because they are
  // dependencies of other shared libraries, not because they are part
  // of the interface.
  struct SharedDepEntry
  {
    cmLinkItem Item;
    size_t DependerIndex;
  };
  std::queue<SharedDepEntry> SharedDepQueue;
  std::set<size_t> SharedDepFollowed;
  void FollowSharedDeps(size_t depender_index, cmLinkInterface const* iface,
                        bool follow_interface = false);
  void QueueSharedDependencies(size_t depender_index,
                               std::vector<cmLinkItem> const& deps);
  void HandleSharedDependency(SharedDepEntry const& dep);

  // Dependency inferral for each link item.
  struct DependSet : public std::set<size_t>
  {
  };
  struct DependSetList : public std::vector<DependSet>
  {
    bool Initialized = false;
  };
  std::vector<DependSetList> InferredDependSets;
  void InferDependencies();

  // To finalize dependencies over groups in place of raw items
  void UpdateGroupDependencies();

  // Ordering constraint graph adjacency list.
  using NodeList = cmGraphNodeList;
  using EdgeList = cmGraphEdgeList;
  using Graph = cmGraphAdjacencyList;
  Graph EntryConstraintGraph;
  void CleanConstraintGraph();
  bool CheckCircularDependencies() const;
  void DisplayConstraintGraph();

  // Ordering algorithm.
  void OrderLinkEntries();
  std::vector<char> ComponentVisited;
  std::vector<size_t> ComponentOrder;

  struct PendingComponent
  {
    // The real component id.  Needed because the map is indexed by
    // component topological index.
    size_t Id;

    // The number of times the component needs to be seen.  This is
    // always 1 for trivial components and is initially 2 for
    // non-trivial components.
    size_t Count;

    // The entries yet to be seen to complete the component.
    std::set<size_t> Entries;
  };
  std::map<size_t, PendingComponent> PendingComponents;
  std::unique_ptr<cmComputeComponentGraph> CCG;
  std::vector<size_t> FinalLinkOrder;
  void DisplayComponents();
  void VisitComponent(size_t c);
  void VisitEntry(size_t index);
  PendingComponent& MakePendingComponent(size_t component);
  size_t ComputeComponentCount(NodeList const& nl);
  void DisplayOrderedEntries();
  void DisplayFinalEntries();

  // Record of the original link line.
  std::vector<size_t> OriginalEntries;

  // Record of explicitly linked object files.
  std::vector<size_t> ObjectEntries;

  size_t ComponentOrderId;
};

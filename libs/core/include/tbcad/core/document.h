#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tbcad/core/entities.h"
#include "tbcad/core/result.h"
#include "tbcad/core/tables.h"

namespace tbcad {

// Half-extent of the default working area (+-100 km) and the hard coordinate cap, in mm.
inline constexpr double kDefaultWorkArea = 1e8;
inline constexpr double kHardCap = 1e10;

// Rows every document has. They can be edited (except as noted) but never deleted.
inline constexpr LayerId kLayerZero{1};     // name and id are fixed
inline constexpr LevelId kLevelDefault{1};  // cannot be deleted, can be renamed
inline constexpr LinetypeId kLinetypeContinuous{1};
inline constexpr TextStyleId kTextStyleStandard{1};
inline constexpr DimStyleId kDimStyleStandard{1};

// How objects on levels other than the current one are treated.
enum class LevelView : std::uint8_t {
    DimOthers,   // shown faded, cannot be selected or snapped to (default)
    HideOthers,  // not shown at all
    ShowAll,     // shown normally and fully editable
};

struct Header {
    LengthUnit units = LengthUnit::Millimeter;
    double workArea = kDefaultWorkArea;  // geometry beyond +-workArea is flagged, not rejected
    double ltScale = 1;
    LayerId currentLayer = kLayerZero;
    LevelId currentLevel = kLevelDefault;
    LevelView levelView = LevelView::DimOthers;
    // Properties new objects get from the drawing commands (ByLayer unless the user sets them).
    Color currentColor = Color::byLayer();  // never ByParent
    LinetypeId currentLinetype = kLinetypeByLayer;
    LineWeight currentLineweight = LineWeight::byLayer();
    TextStyleId currentTextStyle = kTextStyleStandard;
    DimStyleId currentDimStyle = kDimStyleStandard;
    // "Active only": every layer except the current layer and its sub-layers behaves as locked
    // (see Document::effective). Session state: not written to the file.
    bool activeOnly = false;
    std::string createdBy{};
    std::map<std::string, std::string> custom{};  // settings stored in the file
};

// Annotation scale: model units per paper unit (100 for 1:100). Kept in Header::custom so it is
// saved with the drawing. Always positive; 1 when unset or damaged.
inline constexpr const char* kAnnoScaleKey = "tbcad.annoscale";
inline constexpr const char* kAnnoScaleListKey = "tbcad.scalelist";
double currentAnnotationScale(const Header& h);
// The scales offered for annotation, as N of 1:N. Default list when none is stored.
std::vector<double> annotationScaleList(const Header& h);
// 1 for ordinary objects; for annotative ones `override` when positive, else the current scale.
double annotationFactor(const Header& h, const EntityCommon& c, double override = 0);

enum class Direction { Undo, Redo };

template <class Row>
struct RowChange {
    std::optional<Row> before{};
    std::optional<Row> after{};
};

struct HeaderChange {
    Header before{};
    Header after{};
};

// Draw-order override (bring to front / send to back). Without an override an entity's key is its
// id, so draw order is creation order.
struct OrderChange {
    EntityId id{};
    std::optional<std::int64_t> before{};
    std::optional<std::int64_t> after{};
};

inline constexpr std::uint32_t kNoSnapshot = 0xFFFFFFFFu;

// Index pair into ChangeSet::snapshots. before == kNoSnapshot: the entity was added;
// after == kNoSnapshot: it was erased.
struct EntityChange {
    std::uint32_t before = kNoSnapshot;
    std::uint32_t after = kNoSnapshot;
};

// A state transition from "before" to "after" for everything one commit touched. Undoing is
// applying inverted().
struct ChangeSet {
    std::string name{};
    std::shared_ptr<const std::vector<Entity>> snapshots = std::make_shared<std::vector<Entity>>();
    std::vector<EntityChange> entities{};
    std::vector<RowChange<Layer>> layers{};
    std::vector<RowChange<Level>> levels{};
    std::vector<RowChange<Linetype>> linetypes{};
    std::vector<RowChange<TextStyle>> textStyles{};
    std::vector<RowChange<DimStyle>> dimStyles{};
    std::vector<RowChange<BlockDef>> blocks{};
    std::optional<HeaderChange> header{};
    std::vector<OrderChange> order{};
    // Entities (added or modified) lying outside the working area, for the app to warn about.
    std::vector<EntityId> outOfArea{};

    const Entity* before(const EntityChange& c) const {
        return c.before == kNoSnapshot ? nullptr : &(*snapshots)[c.before];
    }
    const Entity* after(const EntityChange& c) const {
        return c.after == kNoSnapshot ? nullptr : &(*snapshots)[c.after];
    }
    // Id of the entity a change refers to.
    EntityId idOf(const EntityChange& c) const {
        const Entity* e = c.after != kNoSnapshot ? after(c) : before(c);
        return e->common.id;
    }
    bool empty() const;
    ChangeSet inverted() const;
};

// Called once per commit, undo and redo, after the document has changed. The ChangeSet always
// describes the transition that just happened (for an undo it is the inverted set).
class DocumentObserver {
public:
    virtual ~DocumentObserver() = default;
    virtual void onChanged(const ChangeSet& changes) = 0;
};

namespace detail {

// Chunked storage with stable addresses while the entity count only grows.
class EntityStore {
public:
    EntityStore() = default;
    EntityStore(const EntityStore&) = delete;
    EntityStore& operator=(const EntityStore&) = delete;
    ~EntityStore();

    std::size_t size() const { return size_; }
    Entity& at(std::size_t i) { return chunks_[i / kChunk][i % kChunk]; }
    const Entity& at(std::size_t i) const { return chunks_[i / kChunk][i % kChunk]; }
    std::size_t pushBack(Entity&& e);
    void popBack();

    template <class Fn>
    void forEach(Fn&& fn) const {
        for (std::size_t i = 0; i < size_; ++i)
            fn(at(i));
    }

private:
    static constexpr std::size_t kChunk = 4096;
    std::vector<Entity*> chunks_{};
    std::size_t size_ = 0;
};

template <class Row, class IdT>
struct Table {
    std::map<IdT, Row> rows{};
    std::unordered_map<std::string, IdT> byName{};  // ASCII case-folded names
    std::uint32_t next = 1;
    std::uint64_t version = 0;  // bumped on every row change

    const Row* find(IdT id) const {
        auto it = rows.find(id);
        return it == rows.end() ? nullptr : &it->second;
    }
    const Row* findByName(std::string_view name) const;
    const Row* findByKey(const std::string& key) const {
        auto it = byName.find(key);
        return it == byName.end() ? nullptr : find(it->second);
    }
};

}  // namespace detail

// The next id each id space will hand out. Ids are never reused, so a document that is saved
// must also keep these: the highest id still in use can be lower than the highest id ever given.
struct NextIds {
    std::uint64_t entity = 1;
    std::uint32_t layer = 1;
    std::uint32_t level = 1;
    std::uint32_t linetype = 1;
    std::uint32_t textStyle = 1;
    std::uint32_t dimStyle = 1;
    std::uint32_t block = 1;
};

class Transaction;

class Document {
public:
    Document();
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    const Header& header() const { return header_; }

    // Entities. Pointers are valid until the next mutation of the document.
    const Entity* find(EntityId id) const;
    bool contains(EntityId id) const { return find(id) != nullptr; }
    // All entities including those inside block definitions.
    std::size_t entityCount() const { return store_.size(); }
    // In storage order (not draw order).
    template <class Fn>
    void forEachEntity(Fn&& fn) const {
        store_.forEach(fn);
    }
    // Model-space entity ids in draw order (back to front).
    std::vector<EntityId> modelSpace() const;
    std::size_t modelSpaceCount() const { return store_.size() - owner_.size(); }
    // The draw-order override of an entity (bring to front / send to back), if it has one.
    std::optional<std::int64_t> drawOrderOverride(EntityId id) const {
        auto it = orderOverride_.find(id);
        if (it == orderOverride_.end())
            return std::nullopt;
        return it->second;
    }
    // Block that owns the entity; null for model space.
    BlockId ownerBlock(EntityId id) const;

    // Tables.
    const std::map<LayerId, Layer>& layers() const { return layers_.rows; }
    const std::map<LevelId, Level>& levels() const { return levels_.rows; }
    const std::map<LinetypeId, Linetype>& linetypes() const { return linetypes_.rows; }
    const std::map<TextStyleId, TextStyle>& textStyles() const { return textStyles_.rows; }
    const std::map<DimStyleId, DimStyle>& dimStyles() const { return dimStyles_.rows; }
    const std::map<BlockId, BlockDef>& blocks() const { return blocks_.rows; }
    const Layer* layer(LayerId id) const { return layers_.find(id); }
    const Level* level(LevelId id) const { return levels_.find(id); }
    const Level* levelByName(std::string_view name) const { return levels_.findByName(name); }
    const Linetype* linetype(LinetypeId id) const { return linetypes_.find(id); }
    const TextStyle* textStyle(TextStyleId id) const { return textStyles_.find(id); }
    const DimStyle* dimStyle(DimStyleId id) const { return dimStyles_.find(id); }
    const BlockDef* block(BlockId id) const { return blocks_.find(id); }
    // Names compare ASCII case-insensitively.
    // Top-level layer with this name.
    const Layer* layerByName(std::string_view name) const { return layers_.findByName(name); }
    // Layer state with sub-layer inheritance applied. Cached; the cache is dropped on any
    // layer-table change. An unknown id yields defaults with a null id.
    EffectiveLayer effective(LayerId id) const;  // honours Header::activeOnly
    // The layer state of an object, with its level applied: a hidden level (or, depending on
    // Header::levelView, any level other than the current one) switches the object off; a locked
    // level or a dimmed other level locks it (`otherLevel` is set for the latter).
    EffectiveLayer effective(const EntityCommon& c) const;
    // Child layer with this name under `parent` (null parent = top level).
    const Layer* layerByName(LayerId parent, std::string_view name) const;
    const Linetype* linetypeByName(std::string_view n) const { return linetypes_.findByName(n); }
    const TextStyle* textStyleByName(std::string_view n) const { return textStyles_.findByName(n); }
    const DimStyle* dimStyleByName(std::string_view n) const { return dimStyles_.findByName(n); }
    const BlockDef* blockByName(std::string_view name) const { return blocks_.findByName(name); }

    // Starts the only mutation path. Throws std::logic_error if a transaction is already open.
    Transaction begin(std::string name);
    // Reverts or reapplies a committed change set. Throws std::logic_error during a transaction.
    void apply(const ChangeSet& changes, Direction direction);

    void addObserver(DocumentObserver* observer);
    void removeObserver(DocumentObserver* observer);

    // The next free id of each id space.
    NextIds nextIds() const;
    // Raises the next free ids to at least `ids` (a field that is lower than the current value is
    // left alone). Used by file readers to restore ids that were handed out before the file was
    // saved. Not undoable; fails while a transaction is open.
    Result<void> reserveIds(const NextIds& ids);

    // Incremented by every commit, undo and redo.
    std::uint64_t revision() const { return revision_; }
    // Unique per Document object for the life of the process (addresses get reused, this does not).
    std::uint64_t uid() const { return uid_; }

    // Collaboration. While a window is set, new entities take their ids from [first, limit)
    // instead of the shared counter, so peers that edit the same drawing never mint the same id.
    // The shared counter (nextIds().entity) still rises past every id in use. Adding an entity
    // fails once the window is used up. Fails while a transaction is open.
    Result<void> setEntityIdWindow(std::uint64_t first, std::uint64_t limit);
    void clearEntityIdWindow();
    // Ids left in the window; the largest value when no window is set.
    std::uint64_t entityIdsLeft() const;

    // Applies a change set that came from another peer, as apply(changes, Redo) does, but first
    // checks every entity and row it writes with the rules a Transaction enforces (known layers,
    // unique names, no dangling references ...). A change set that fails is not applied at all,
    // and observers are not told. `changes` must carry the true "before" state of the document.
    Result<void> applyChecked(const ChangeSet& changes);

private:
    friend class Transaction;

    void applyForward(const ChangeSet& cs);
    void notify(const ChangeSet& cs);
    Entity* findMutable(EntityId id);
    void insertEntity(Entity&& e);  // e.common.id must be set
    void removeEntity(EntityId id);
    void setBlockRow(const BlockDef* oldRow, const BlockDef* newRow);
    std::int64_t orderKey(EntityId id) const;

    Header header_{};
    detail::EntityStore store_{};
    std::vector<std::uint32_t> posOf_{};  // indexed by EntityId::value
    std::uint64_t nextEntityId_ = 1;
    std::uint64_t windowNext_ = 0, windowLimit_ = 0;  // entity id window; limit 0 = none
    // Where the next new entity id comes from.
    std::uint64_t allocCursor() const { return windowLimit_ ? windowNext_ : nextEntityId_; }
    std::unordered_map<EntityId, BlockId> owner_{};  // entities that live in a block definition
    std::map<EntityId, std::int64_t> orderOverride_{};

    detail::Table<Layer, LayerId> layers_{};
    detail::Table<Level, LevelId> levels_{};
    detail::Table<Linetype, LinetypeId> linetypes_{};
    detail::Table<TextStyle, TextStyleId> textStyles_{};
    detail::Table<DimStyle, DimStyleId> dimStyles_{};
    detail::Table<BlockDef, BlockId> blocks_{};

    EffectiveLayer withActiveOnly(EffectiveLayer e) const;
    mutable std::unordered_map<LayerId, EffectiveLayer> effectiveCache_{};
    mutable std::uint64_t effectiveVersion_ = 0;

    std::vector<DocumentObserver*> observers_{};
    Transaction* active_ = nullptr;
    std::uint64_t revision_ = 0;
    std::uint64_t uid_ = 0;
};

// All edits of a Document go through a Transaction. Edits are applied immediately (reads inside
// the transaction see them) but observers are told only on commit(). A transaction that is
// destroyed without commit() rolls everything back.
class Transaction {
public:
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    // Entities. `owner` null = model space, otherwise the block definition that gets the entity.
    Result<EntityId> add(Entity entity, BlockId owner = {});
    Result<void> modify(EntityId id, const std::function<void(Entity&)>& fn);
    Result<void> erase(EntityId id);
    Result<void> bringToFront(EntityId id);
    Result<void> sendToBack(EntityId id);

    // Tables. `add*` ignores the row's id and returns the assigned one.
    Result<LayerId> addLayer(Layer row);
    Result<void> modifyLayer(LayerId id, const std::function<void(Layer&)>& fn);
    Result<void> eraseLayer(LayerId id);
    // The default level cannot be deleted; a level that has objects (or is current) cannot be
    // deleted either: move the objects first.
    Result<LevelId> addLevel(Level row);
    Result<void> modifyLevel(LevelId id, const std::function<void(Level&)>& fn);
    Result<void> eraseLevel(LevelId id);
    Result<LinetypeId> addLinetype(Linetype row);
    Result<void> modifyLinetype(LinetypeId id, const std::function<void(Linetype&)>& fn);
    Result<void> eraseLinetype(LinetypeId id);
    Result<TextStyleId> addTextStyle(TextStyle row);
    Result<void> modifyTextStyle(TextStyleId id, const std::function<void(TextStyle&)>& fn);
    Result<void> eraseTextStyle(TextStyleId id);
    Result<DimStyleId> addDimStyle(DimStyle row);
    Result<void> modifyDimStyle(DimStyleId id, const std::function<void(DimStyle&)>& fn);
    Result<void> eraseDimStyle(DimStyleId id);
    // The entity list of a block is changed through add(..., owner) and erase(); modifyBlock
    // ignores edits to `entities`. eraseBlock also erases the block's entities and fails while an
    // Insert references the block.
    Result<BlockId> addBlock(BlockDef row);
    Result<void> modifyBlock(BlockId id, const std::function<void(BlockDef&)>& fn);
    Result<void> eraseBlock(BlockId id);

    Result<void> modifyHeader(const std::function<void(Header&)>& fn);

    // Ids of the entities that existed when the transaction began and have been modified or erased
    // by it so far (entities added in this transaction are not listed). Sorted by id. Lets
    // pre-commit hooks find out what an edit touched, e.g. to update dependent objects.
    std::vector<EntityId> touchedEntities() const;

    // The document this transaction edits; reads see the edits made so far.
    const Document& document() const { return *doc_; }

    // Finishes the transaction. Observers are notified once unless nothing changed.
    ChangeSet commit();
    // Undoes everything done so far and ends the transaction.
    void rollback();

private:
    friend class Document;
    Transaction(Document* doc, std::string name);

    template <class Row, class IdT>
    struct Pending {
        std::map<IdT, RowChange<Row>> rows{};
    };

    template <class Row, class IdT>
    Result<IdT> addRow(detail::Table<Row, IdT>& t, Pending<Row, IdT>& p, Row row);
    template <class Row, class IdT>
    Result<void> modifyRow(detail::Table<Row, IdT>& t, Pending<Row, IdT>& p, IdT id,
                           const std::function<void(Row&)>& fn);
    template <class Row, class IdT>
    Result<void> eraseRow(detail::Table<Row, IdT>& t, Pending<Row, IdT>& p, IdT id);
    template <class Row, class IdT>
    void touchRow(detail::Table<Row, IdT>& t, Pending<Row, IdT>& p, IdT id);

    void touchEntity(EntityId id);
    void touchBlock(BlockId id);
    void touchOrder(EntityId id);
    Document& doc() const;
    Result<void> eraseEntityInternal(EntityId id);

    Document* doc_ = nullptr;
    std::string name_{};
    std::uint64_t startNextId_ = 0;
    std::vector<Entity> beforeSnaps_{};                      // pre-existing entities touched
    std::unordered_map<EntityId, std::uint32_t> touched_{};  // id -> index into beforeSnaps_
    std::vector<EntityId> outOfArea_{};
    std::optional<Header> headerBefore_{};
    std::map<EntityId, OrderChange> order_{};
    Pending<Layer, LayerId> pLayers_{};
    Pending<Level, LevelId> pLevels_{};
    Pending<Linetype, LinetypeId> pLinetypes_{};
    Pending<TextStyle, TextStyleId> pTextStyles_{};
    Pending<DimStyle, DimStyleId> pDimStyles_{};
    Pending<BlockDef, BlockId> pBlocks_{};
};

// New objects the user draws take the layer and the current colour, linetype and lineweight of the
// drawing (Header::current*). Commands that copy or edit existing objects keep their own.
inline void applyCurrentProperties(const Header& h, Entity& e) {
    e.common.layer = h.currentLayer;
    e.common.color = h.currentColor;
    e.common.linetype = h.currentLinetype;
    e.common.lineweight = h.currentLineweight;
}

}  // namespace tbcad

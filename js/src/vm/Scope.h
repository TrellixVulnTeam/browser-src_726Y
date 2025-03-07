/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Scope_h
#define vm_Scope_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_ASSERT_IF
#include "mozilla/Attributes.h"  // MOZ_IMPLICIT, MOZ_INIT_OUTSIDE_CTOR, MOZ_STACK_CLASS
#include "mozilla/Casting.h"          // mozilla::AssertedCast
#include "mozilla/Maybe.h"            // mozilla::Maybe
#include "mozilla/MemoryReporting.h"  // mozilla::MallocSizeOf

#include <algorithm>    // std::fill_n
#include <stddef.h>     // size_t
#include <stdint.h>     // uint8_t, uint16_t, uint32_t, uintptr_t
#include <type_traits>  // std::is_same_v

#include "builtin/ModuleObject.h"  // ModuleObject, HandleModuleObject
#include "frontend/ParserAtom.h"   // frontend::TaggedParserAtomIndex
#include "gc/Allocator.h"          // AllowGC
#include "gc/Barrier.h"            // HeapPtr
#include "gc/Cell.h"               // TenuredCellWithNonGCPointer
#include "gc/MaybeRooted.h"        // MaybeRooted
#include "gc/Rooting.h"      // HandleScope, HandleShape, MutableHandleShape
#include "js/GCPolicyAPI.h"  // GCPolicy, IgnoreGCPolicy
#include "js/HeapAPI.h"      // CellFlagBitsReservedForGC
#include "js/RootingAPI.h"   // Handle, MutableHandle
#include "js/TraceKind.h"    // JS::TraceKind
#include "js/TypeDecls.h"    // HandleFunction
#include "js/UbiNode.h"      // ubi::*
#include "js/UniquePtr.h"    // UniquePtr
#include "util/Poison.h"  // AlwaysPoison, JS_SCOPE_DATA_TRAILING_NAMES_PATTERN, MemCheckKind
#include "vm/BytecodeUtil.h"  // LOCALNO_LIMIT, ENVCOORD_SLOT_LIMIT
#include "vm/JSFunction.h"    // JSFunction
#include "vm/ScopeKind.h"     // ScopeKind
#include "vm/Shape.h"         // Shape
#include "vm/Xdr.h"           // XDRResult, XDRState
#include "wasm/WasmJS.h"      // WasmInstanceObject

class JSAtom;
class JSFreeOp;
class JSFunction;
class JSScript;
class JSTracer;
struct JSContext;

namespace JS {
class Zone;
}  // namespace JS

namespace js {

class GenericPrinter;

namespace frontend {
struct CompilationAtomCache;
class ScopeStencil;
class ParserAtom;
}  // namespace frontend

template <typename NameT>
class AbstractBaseScopeData;

template <typename NameT>
class BaseAbstractBindingIter;

template <typename NameT>
class AbstractBindingIter;

using BindingIter = AbstractBindingIter<JSAtom>;

class AbstractScopePtr;

static inline bool ScopeKindIsCatch(ScopeKind kind) {
  return kind == ScopeKind::SimpleCatch || kind == ScopeKind::Catch;
}

static inline bool ScopeKindIsInBody(ScopeKind kind) {
  return kind == ScopeKind::Lexical || kind == ScopeKind::SimpleCatch ||
         kind == ScopeKind::Catch || kind == ScopeKind::With ||
         kind == ScopeKind::FunctionLexical ||
         kind == ScopeKind::FunctionBodyVar || kind == ScopeKind::ClassBody;
}

const char* BindingKindString(BindingKind kind);
const char* ScopeKindString(ScopeKind kind);

template <typename NameT>
class AbstractBindingName;

template <>
class AbstractBindingName<JSAtom> {
 public:
  using NameT = JSAtom;
  using NamePointerT = NameT*;

 private:
  // A JSAtom* with its low bit used as a tag for the:
  //  * whether it is closed over (i.e., exists in the environment shape)
  //  * whether it is a top-level function binding in global or eval scope,
  //    instead of var binding (both are in the same range in Scope data)
  uintptr_t bits_;

  static constexpr uintptr_t ClosedOverFlag = 0x1;
  // TODO: We should reuse this bit for let vs class distinction to
  //       show the better redeclaration error message (bug 1428672).
  static constexpr uintptr_t TopLevelFunctionFlag = 0x2;
  static constexpr uintptr_t FlagMask = 0x3;

 public:
  AbstractBindingName() : bits_(0) {}

  AbstractBindingName(NameT* name, bool closedOver,
                      bool isTopLevelFunction = false)
      : bits_(uintptr_t(name) | (closedOver ? ClosedOverFlag : 0x0) |
              (isTopLevelFunction ? TopLevelFunctionFlag : 0x0)) {}

 private:
  // For fromXDR.
  AbstractBindingName(NameT* name, uint8_t flags)
      : bits_(uintptr_t(name) | flags) {
    static_assert(FlagMask < alignof(NameT),
                  "Flags should fit into unused low bits of atom repr");
    MOZ_ASSERT((flags & FlagMask) == flags);
  }

 public:
  static AbstractBindingName<NameT> fromXDR(NameT* name, uint8_t flags) {
    return AbstractBindingName<NameT>(name, flags);
  }

  uint8_t flagsForXDR() const { return static_cast<uint8_t>(bits_ & FlagMask); }

  NamePointerT name() const {
    return reinterpret_cast<NameT*>(bits_ & ~FlagMask);
  }

  bool closedOver() const { return bits_ & ClosedOverFlag; }

 private:
  friend class BaseAbstractBindingIter<NameT>;

  // This method should be called only for binding names in `vars` range in
  // BindingIter.
  bool isTopLevelFunction() const { return bits_ & TopLevelFunctionFlag; }

 public:
  void trace(JSTracer* trc);
};

template <>
class AbstractBindingName<frontend::TaggedParserAtomIndex> {
  uint32_t bits_;

  using TaggedParserAtomIndex = frontend::TaggedParserAtomIndex;

 public:
  using NameT = TaggedParserAtomIndex;
  using NamePointerT = NameT;

 private:
  static constexpr size_t TaggedIndexBit = TaggedParserAtomIndex::IndexBit + 2;

  static constexpr size_t FlagShift = TaggedIndexBit;
  static constexpr size_t FlagBit = 2;
  static constexpr uint32_t FlagMask = BitMask(FlagBit) << FlagShift;

  static constexpr uint32_t ClosedOverFlag = 1 << FlagShift;
  static constexpr uint32_t TopLevelFunctionFlag = 2 << FlagShift;

 public:
  AbstractBindingName() : bits_(TaggedParserAtomIndex::NullTag) {
    // TaggedParserAtomIndex's tags shouldn't overlap with flags.
    static_assert((TaggedParserAtomIndex::NullTag & FlagMask) == 0);
    static_assert((TaggedParserAtomIndex::ParserAtomIndexTag & FlagMask) == 0);
    static_assert((TaggedParserAtomIndex::WellKnownTag & FlagMask) == 0);
  }

  AbstractBindingName(TaggedParserAtomIndex name, bool closedOver,
                      bool isTopLevelFunction = false)
      : bits_(name.rawData() | (closedOver ? ClosedOverFlag : 0x0) |
              (isTopLevelFunction ? TopLevelFunctionFlag : 0x0)) {}

 public:
  NamePointerT name() const {
    return TaggedParserAtomIndex::fromRaw(bits_ & ~FlagMask);
  }

  bool closedOver() const { return bits_ & ClosedOverFlag; }

  AbstractBindingName<JSAtom> copyWithNewAtom(JSAtom* newName) const {
    return AbstractBindingName<JSAtom>(newName, closedOver(),
                                       isTopLevelFunction());
  }

 private:
  friend class BaseAbstractBindingIter<TaggedParserAtomIndex>;
  friend class frontend::ScopeStencil;

  // This method should be called only for binding names in `vars` range in
  // BindingIter.
  bool isTopLevelFunction() const { return bits_ & TopLevelFunctionFlag; }
};

using BindingName = AbstractBindingName<JSAtom>;

const size_t ScopeDataAlignBytes = size_t(1) << gc::CellFlagBitsReservedForGC;

/**
 * Empty base class for scope {Runtime,Parser}Data classes to inherit from.
 *
 * Scope GC things store a pointer to these in their first word so they must be
 * suitably aligned to allow storing GC flags in the low bits.
 */
template <typename NameT>
class AbstractBaseScopeData {
 public:
  using NameType = NameT;
};

using BaseScopeData = AbstractBaseScopeData<JSAtom>;

inline void PoisonNames(AbstractBindingName<JSAtom>* data, size_t nameCount) {
  AlwaysPoison(data, JS_SCOPE_DATA_TRAILING_NAMES_PATTERN,
               sizeof(AbstractBindingName<JSAtom>) * nameCount,
               MemCheckKind::MakeUndefined);
}

// frontend::TaggedParserAtomIndex doesn't require poison value.
// Fill with null value instead.
inline void PoisonNames(
    AbstractBindingName<frontend::TaggedParserAtomIndex>* data,
    size_t nameCount) {
  std::fill_n(data, nameCount,
              AbstractBindingName<frontend::TaggedParserAtomIndex>());
}

/**
 * The various {Global,Module,...}Scope::{Runtime,Parser}Data classes consist
 * of always-present bits, then a trailing array of BindingNames.  The various
 * {Runtime,Parser}Data classes all end in a TrailingNamesArray that contains
 * sized/aligned space for *one* BindingName.  {Runtime,Parser}Data instances
 * that contain N BindingNames, are then allocated in
 * sizeof({Runtime,Parser}Data) + (space for (N - 1) BindingNames).
 * Because this class's |data_| field is properly sized/aligned, the
 * N-BindingName array can start at |data_|.
 *
 * This is concededly a very low-level representation, but we want to only
 * allocate once for data+bindings both, and this does so approximately as
 * elegantly as C++ allows.
 *
 * The names array is implemented in terms of an generic type that
 * allows specialization between a (JSAtom*) BindingName and a
 * ParserAtom
 */
template <typename NameT>
class AbstractTrailingNamesArray {
  using BindingNameT = AbstractBindingName<NameT>;

 private:
  alignas(BindingNameT) unsigned char data_[sizeof(BindingNameT)];

 private:
  // Some versions of GCC treat it as a -Wstrict-aliasing violation (ergo a
  // -Werror compile error) to reinterpret_cast<> |data_| to |T*|, even
  // through |void*|.  Placing the latter cast in these separate functions
  // breaks the chain such that affected GCC versions no longer warn/error.
  void* ptr() { return data_; }
  const void* ptr() const { return data_; }

 public:
  // Explicitly ensure no one accidentally allocates scope data without
  // poisoning its trailing names.
  AbstractTrailingNamesArray() = delete;

  explicit AbstractTrailingNamesArray(size_t nameCount) {
    if (nameCount) {
      PoisonNames(reinterpret_cast<BindingNameT*>(&data_), nameCount);
    }
  }

  BindingNameT* start() { return reinterpret_cast<BindingNameT*>(ptr()); }
  const BindingNameT* start() const {
    return reinterpret_cast<const BindingNameT*>(ptr());
  }

  BindingNameT& get(size_t i) { return start()[i]; }
  const BindingNameT& get(size_t i) const { return start()[i]; }
  BindingNameT& operator[](size_t i) { return get(i); }
  const BindingNameT& operator[](size_t i) const { return get(i); }
};

//
// Allow using is<T> and as<T> on Rooted<Scope*> and Handle<Scope*>.
//
template <typename Wrapper>
class WrappedPtrOperations<Scope*, Wrapper> {
 public:
  template <class U>
  JS::Handle<U*> as() const {
    const Wrapper& self = *static_cast<const Wrapper*>(this);
    MOZ_ASSERT_IF(self, self->template is<U>());
    return Handle<U*>::fromMarkedLocation(
        reinterpret_cast<U* const*>(self.address()));
  }
};

//
// The base class of all Scopes.
//
class Scope : public gc::TenuredCellWithNonGCPointer<BaseScopeData> {
  friend class GCMarker;
  friend class frontend::ScopeStencil;
  friend class js::AbstractBindingIter<JSAtom>;

 protected:
  // The raw data pointer, stored in the cell header.
  BaseScopeData* rawData() { return headerPtr(); }
  const BaseScopeData* rawData() const { return headerPtr(); }

  // The kind determines data_.
  const ScopeKind kind_;

  // If there are any aliased bindings, the shape for the
  // EnvironmentObject. Otherwise nullptr.
  const HeapPtr<Shape*> environmentShape_;

  // The enclosing scope or nullptr.
  HeapPtr<Scope*> enclosingScope_;

  Scope(ScopeKind kind, Scope* enclosing, Shape* environmentShape)
      : TenuredCellWithNonGCPointer(nullptr),
        kind_(kind),
        environmentShape_(environmentShape),
        enclosingScope_(enclosing) {}

  static Scope* create(JSContext* cx, ScopeKind kind, HandleScope enclosing,
                       HandleShape envShape);

  template <typename ConcreteScope, XDRMode mode>
  static XDRResult XDRSizedBindingNames(
      XDRState<mode>* xdr, Handle<ConcreteScope*> scope,
      MutableHandle<typename ConcreteScope::RuntimeData*> data);

  Shape* maybeCloneEnvironmentShape(JSContext* cx);

  template <typename ConcreteScope>
  void initData(
      MutableHandle<UniquePtr<typename ConcreteScope::RuntimeData>> data);

  template <typename F>
  void applyScopeDataTyped(F&& f);

  template <typename EnvironmentT>
  static bool updateEnvShapeIfRequired(JSContext* cx, MutableHandleShape shape,
                                       bool needsEnvironment);

  template <typename EnvironmentT>
  static bool updateEnvShapeIfRequired(JSContext* cx,
                                       mozilla::Maybe<uint32_t>* envShape,
                                       bool needsEnvironment);

 public:
  template <typename ConcreteScope>
  static ConcreteScope* create(
      JSContext* cx, ScopeKind kind, HandleScope enclosing,
      HandleShape envShape,
      MutableHandle<UniquePtr<typename ConcreteScope::RuntimeData>> data);

  static const JS::TraceKind TraceKind = JS::TraceKind::Scope;

  template <typename T>
  bool is() const {
    return kind_ == T::classScopeKind_;
  }

  template <typename T>
  T& as() {
    MOZ_ASSERT(this->is<T>());
    return *static_cast<T*>(this);
  }

  template <typename T>
  const T& as() const {
    MOZ_ASSERT(this->is<T>());
    return *static_cast<const T*>(this);
  }

  ScopeKind kind() const { return kind_; }

  Shape* environmentShape() const { return environmentShape_; }

  Scope* enclosing() const { return enclosingScope_; }

  static bool hasEnvironment(ScopeKind kind, bool environmentShape) {
    switch (kind) {
      case ScopeKind::With:
      case ScopeKind::Global:
      case ScopeKind::NonSyntactic:
        return true;
      default:
        // If there's a shape, an environment must be created for this scope.
        return environmentShape;
    }
  }

  bool hasEnvironment() const {
    return hasEnvironment(kind_, environmentShape());
  }

  uint32_t firstFrameSlot() const;

  uint32_t chainLength() const;
  uint32_t environmentChainLength() const;

  template <typename T>
  bool hasOnChain() const {
    for (const Scope* it = this; it; it = it->enclosing()) {
      if (it->is<T>()) {
        return true;
      }
    }
    return false;
  }

  bool hasOnChain(ScopeKind kind) const {
    for (const Scope* it = this; it; it = it->enclosing()) {
      if (it->kind() == kind) {
        return true;
      }
    }
    return false;
  }

  static Scope* clone(JSContext* cx, HandleScope scope, HandleScope enclosing);

  void traceChildren(JSTracer* trc);
  void finalize(JSFreeOp* fop);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  void dump();
#if defined(DEBUG) || defined(JS_JITSPEW)
  static bool dumpForDisassemble(JSContext* cx, JS::Handle<Scope*> scope,
                                 GenericPrinter& out, const char* indent);
#endif /* defined(DEBUG) || defined(JS_JITSPEW) */
};

template <class DataT>
inline size_t SizeOfScopeData(uint32_t numBindings) {
  using BindingT = AbstractBindingName<typename DataT::NameType>;

#if JS_BITS_PER_WORD == 64
  static_assert(
      sizeof(DataT) == offsetof(DataT, trailingNames) + sizeof(BindingT),
      "Unexpected default number of inlined elements");
  // -1 because AbstractTrailingNamesArray (trailingNames field in DataT)
  // contains one inlined element in data_ field.
  return sizeof(DataT) +
         ((numBindings ? numBindings - 1 : 0) * sizeof(BindingT));
#else
  // RuntimeData has alignas(ScopeDataAlignBytes), that is 8-bytes.
  // RuntimeData on 32-bit arch may have 4-bytes trailing padding after
  // trailingNames, and in that case there are effectively 2 inlined elements
  // inside sizeof(DataT).
  static_assert(
      sizeof(DataT) == offsetof(DataT, trailingNames) + sizeof(BindingT) ||
          sizeof(DataT) ==
              offsetof(DataT, trailingNames) + 2 * sizeof(BindingT),
      "Unexpected default number of inlined elements");

  if constexpr (sizeof(DataT) ==
                offsetof(DataT, trailingNames) + sizeof(BindingT)) {
    // There's no trailing padding, and there's only one inlined element.
    // This is RuntimeData without padding, or ParserData.
    return sizeof(DataT) +
           ((numBindings ? numBindings - 1 : 0) * sizeof(BindingT));
  }

  // There's trailing padding, and there are two inlined elements.
  return sizeof(DataT) +
         ((numBindings > 2 ? numBindings - 2 : 0) * sizeof(BindingT));
#endif
}

//
// A useful typedef for selecting between a gc-aware wrappers
// around pointers to BaseScopeData-derived types, and around raw
// pointer wrappers around BaseParserScopeData-derived types.
//
template <typename ScopeT, typename AtomT>
using AbstractScopeData = typename ScopeT::template AbstractData<AtomT>;

template <typename ScopeT, typename AtomT>
using MaybeRootedScopeData = std::conditional_t<
    std::is_same_v<AtomT, JSAtom>,
    MaybeRooted<UniquePtr<typename ScopeT::RuntimeData>, AllowGC::CanGC>,
    MaybeRooted<AbstractScopeData<ScopeT, AtomT>*, AllowGC::NoGC>>;

template <typename SlotInfo>
struct ParserScopeData
    : public AbstractBaseScopeData<frontend::TaggedParserAtomIndex> {
  SlotInfo slotInfo;
  AbstractTrailingNamesArray<frontend::TaggedParserAtomIndex> trailingNames;

  explicit ParserScopeData(size_t nameCount) : trailingNames(nameCount) {}
  ParserScopeData() = delete;
};

//
// A lexical scope that holds let and const bindings. There are 4 kinds of
// LexicalScopes.
//
// Lexical
//   A plain lexical scope.
//
// SimpleCatch
//   Holds the single catch parameter of a catch block.
//
// Catch
//   Holds the catch parameters (and only the catch parameters) of a catch
//   block.
//
// NamedLambda
// StrictNamedLambda
//   Holds the single name of the callee for a named lambda expression.
//
// All kinds of LexicalScopes correspond to LexicalEnvironmentObjects on the
// environment chain.
//
class LexicalScope : public Scope {
  friend class Scope;
  friend class AbstractBindingIter<JSAtom>;
  friend class GCMarker;
  friend class frontend::ScopeStencil;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // Bindings are sorted by kind in both frames and environments.
    //
    //   lets - [0, constStart)
    // consts - [constStart, length)
    uint32_t constStart = 0;
    uint32_t length = 0;
  };

  struct alignas(ScopeDataAlignBytes) RuntimeData
      : public AbstractBaseScopeData<JSAtom> {
    SlotInfo slotInfo;
    AbstractTrailingNamesArray<JSAtom> trailingNames;

    explicit RuntimeData(size_t nameCount) : trailingNames(nameCount) {}
    RuntimeData() = delete;

    void trace(JSTracer* trc);
  };

  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  template <XDRMode mode>
  static XDRResult XDR(XDRState<mode>* xdr, ScopeKind kind,
                       HandleScope enclosing, MutableHandleScope scope);

 private:
  static LexicalScope* createWithData(
      JSContext* cx, ScopeKind kind, MutableHandle<UniquePtr<RuntimeData>> data,
      uint32_t firstFrameSlot, HandleScope enclosing);

  template <typename AtomT, typename ShapeT>
  static bool prepareForScopeCreation(
      JSContext* cx, ScopeKind kind, uint32_t firstFrameSlot,
      typename MaybeRootedScopeData<LexicalScope, AtomT>::MutableHandleType
          data,
      ShapeT envShape);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }
  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

  static uint32_t nextFrameSlot(Scope* scope);

 public:
  uint32_t nextFrameSlot() const { return data().slotInfo.nextFrameSlot; }

  // Returns an empty shape for extensible global and non-syntactic lexical
  // scopes.
  static Shape* getEmptyExtensibleEnvironmentShape(JSContext* cx);
};

template <>
inline bool Scope::is<LexicalScope>() const {
  return kind_ == ScopeKind::Lexical || kind_ == ScopeKind::SimpleCatch ||
         kind_ == ScopeKind::Catch || kind_ == ScopeKind::NamedLambda ||
         kind_ == ScopeKind::StrictNamedLambda ||
         kind_ == ScopeKind::FunctionLexical || kind_ == ScopeKind::ClassBody;
}

//
// Scope corresponding to a function. Holds formal parameter names, special
// internal names (see FunctionScope::isSpecialName), and, if the function
// parameters contain no expressions that might possibly be evaluated, the
// function's var bindings. For example, in these functions, the FunctionScope
// will store a/b/c bindings but not d/e/f bindings:
//
//   function f1(a, b) {
//     var c;
//     let e;
//     const f = 3;
//   }
//   function f2([a], b = 4, ...c) {
//     var d, e, f; // stored in VarScope
//   }
//
// Corresponds to CallObject on environment chain.
//
class FunctionScope : public Scope {
  friend class GCMarker;
  friend class AbstractBindingIter<JSAtom>;
  friend class PositionalFormalParameterIter;
  friend class Scope;
  friend class AbstractScopePtr;
  static const ScopeKind classScopeKind_ = ScopeKind::Function;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // Flag bits.
    // This uses uint32_t in order to make this struct packed.
    uint32_t flags = 0;

    // If parameter expressions are present, parameters act like lexical
    // bindings.
    static constexpr uint32_t HasParameterExprsFlag = 1;

    // Bindings are sorted by kind in both frames and environments.
    //
    // Positional formal parameter names are those that are not
    // destructured. They may be referred to by argument slots if
    // !script()->hasParameterExprs().
    //
    // An argument slot that needs to be skipped due to being destructured
    // or having defaults will have a nullptr name in the name array to
    // advance the argument slot.
    //
    // Rest parameter binding is also included in positional formals.
    // This also becomes nullptr if destructuring.
    //
    // The number of positional formals is equal to function.length if
    // there's no rest, function.length+1 otherwise.
    //
    // Destructuring parameters and destructuring rest are included in
    // "other formals" below.
    //
    // "vars" contains the following:
    //   * function's top level vars if !script()->hasParameterExprs()
    //   * special internal names (arguments, .this, .generator) if
    //     they're used.
    //
    // positional formals - [0, nonPositionalFormalStart)
    //      other formals - [nonPositionalParamStart, varStart)
    //               vars - [varStart, length)
    uint16_t nonPositionalFormalStart = 0;
    uint16_t varStart = 0;
    uint32_t length = 0;

    bool hasParameterExprs() const { return flags & HasParameterExprsFlag; }
    void setHasParameterExprs() { flags |= HasParameterExprsFlag; }
  };

  struct alignas(ScopeDataAlignBytes) RuntimeData
      : public AbstractBaseScopeData<JSAtom> {
    // The canonical function of the scope, as during a scope walk we
    // often query properties of the JSFunction (e.g., is the function an
    // arrow).
    HeapPtr<JSFunction*> canonicalFunction = {};
    SlotInfo slotInfo;
    AbstractTrailingNamesArray<JSAtom> trailingNames;

    explicit RuntimeData(size_t nameCount) : trailingNames(nameCount) {}
    RuntimeData() = delete;

    void trace(JSTracer* trc);
  };

  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  template <typename AtomT, typename ShapeT>
  static bool prepareForScopeCreation(
      JSContext* cx,
      typename MaybeRootedScopeData<FunctionScope, AtomT>::MutableHandleType
          data,
      bool hasParameterExprs, bool needsEnvironment, HandleFunction fun,
      ShapeT envShape);

  static FunctionScope* clone(JSContext* cx, Handle<FunctionScope*> scope,
                              HandleFunction fun, HandleScope enclosing);

  template <XDRMode mode>
  static XDRResult XDR(XDRState<mode>* xdr, HandleFunction fun,
                       HandleScope enclosing, MutableHandleScope scope);

 private:
  static FunctionScope* createWithData(
      JSContext* cx, MutableHandle<UniquePtr<RuntimeData>> data,
      bool hasParameterExprs, bool needsEnvironment, HandleFunction fun,
      HandleScope enclosing);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  uint32_t nextFrameSlot() const { return data().slotInfo.nextFrameSlot; }

  JSFunction* canonicalFunction() const { return data().canonicalFunction; }
  void initCanonicalFunction(JSFunction* fun) {
    data().canonicalFunction.init(fun);
  }

  JSScript* script() const;

  bool hasParameterExprs() const { return data().slotInfo.hasParameterExprs(); }

  uint32_t numPositionalFormalParameters() const {
    return data().slotInfo.nonPositionalFormalStart;
  }

  static bool isSpecialName(JSContext* cx, JSAtom* name);
  static bool isSpecialName(JSContext* cx,
                            frontend::TaggedParserAtomIndex name);
};

//
// Scope holding only vars. There is a single kind of VarScopes.
//
// FunctionBodyVar
//   Corresponds to the extra var scope present in functions with parameter
//   expressions. See examples in comment above FunctionScope.
//
// Corresponds to VarEnvironmentObject on environment chain.
//
class VarScope : public Scope {
  friend class GCMarker;
  friend class AbstractBindingIter<JSAtom>;
  friend class Scope;
  friend class frontend::ScopeStencil;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // All bindings are vars.
    //
    //            vars - [0, length)
    uint32_t length = 0;
  };

  struct alignas(ScopeDataAlignBytes) RuntimeData
      : public AbstractBaseScopeData<JSAtom> {
    SlotInfo slotInfo;
    AbstractTrailingNamesArray<JSAtom> trailingNames;

    explicit RuntimeData(size_t nameCount) : trailingNames(nameCount) {}
    RuntimeData() = delete;

    void trace(JSTracer* trc);
  };

  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  template <XDRMode mode>
  static XDRResult XDR(XDRState<mode>* xdr, ScopeKind kind,
                       HandleScope enclosing, MutableHandleScope scope);

 private:
  static VarScope* createWithData(JSContext* cx, ScopeKind kind,
                                  MutableHandle<UniquePtr<RuntimeData>> data,
                                  uint32_t firstFrameSlot,
                                  bool needsEnvironment, HandleScope enclosing);

  template <typename AtomT, typename ShapeT>
  static bool prepareForScopeCreation(
      JSContext* cx, ScopeKind kind,
      typename MaybeRootedScopeData<VarScope, AtomT>::MutableHandleType data,
      uint32_t firstFrameSlot, bool needsEnvironment, ShapeT envShape);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  uint32_t nextFrameSlot() const { return data().slotInfo.nextFrameSlot; }
};

template <>
inline bool Scope::is<VarScope>() const {
  return kind_ == ScopeKind::FunctionBodyVar;
}

//
// Scope corresponding to both the global object scope and the global lexical
// scope.
//
// Both are extensible and are singletons across <script> tags, so these
// scopes are a fragment of the names in global scope. In other words, two
// global scripts may have two different GlobalScopes despite having the same
// GlobalObject.
//
// There are 2 kinds of GlobalScopes.
//
// Global
//   Corresponds to a GlobalObject and its global LexicalEnvironmentObject on
//   the environment chain.
//
// NonSyntactic
//   Corresponds to a non-GlobalObject created by the embedding on the
//   environment chain. This distinction is important for optimizations.
//
class GlobalScope : public Scope {
  friend class Scope;
  friend class AbstractBindingIter<JSAtom>;
  friend class GCMarker;

 public:
  struct SlotInfo {
    // Bindings are sorted by kind.
    // `vars` includes top-level functions which is distinguished by a bit
    // on the BindingName.
    //
    //            vars - [0, letStart)
    //            lets - [letStart, constStart)
    //          consts - [constStart, length)
    uint32_t letStart = 0;
    uint32_t constStart = 0;
    uint32_t length = 0;
  };

  struct alignas(ScopeDataAlignBytes) RuntimeData
      : public AbstractBaseScopeData<JSAtom> {
    SlotInfo slotInfo;
    AbstractTrailingNamesArray<JSAtom> trailingNames;

    explicit RuntimeData(size_t nameCount) : trailingNames(nameCount) {}
    RuntimeData() = delete;

    void trace(JSTracer* trc);
  };

  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  static GlobalScope* create(JSContext* cx, ScopeKind kind,
                             Handle<RuntimeData*> data);

  static GlobalScope* createEmpty(JSContext* cx, ScopeKind kind) {
    return create(cx, kind, nullptr);
  }

  static GlobalScope* clone(JSContext* cx, Handle<GlobalScope*> scope);

  template <XDRMode mode>
  static XDRResult XDR(XDRState<mode>* xdr, ScopeKind kind,
                       MutableHandleScope scope);

 private:
  static GlobalScope* createWithData(
      JSContext* cx, ScopeKind kind,
      MutableHandle<UniquePtr<RuntimeData>> data);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  bool isSyntactic() const { return kind() != ScopeKind::NonSyntactic; }

  bool hasBindings() const { return data().slotInfo.length > 0; }
};

template <>
inline bool Scope::is<GlobalScope>() const {
  return kind_ == ScopeKind::Global || kind_ == ScopeKind::NonSyntactic;
}

//
// Scope of a 'with' statement. Has no bindings.
//
// Corresponds to a WithEnvironmentObject on the environment chain.
class WithScope : public Scope {
  friend class Scope;
  friend class AbstractScopePtr;
  static const ScopeKind classScopeKind_ = ScopeKind::With;

 public:
  static WithScope* create(JSContext* cx, HandleScope enclosing);

  template <XDRMode mode>
  static XDRResult XDR(XDRState<mode>* xdr, HandleScope enclosing,
                       MutableHandleScope scope);
};

//
// Scope of an eval. Holds var bindings. There are 2 kinds of EvalScopes.
//
// StrictEval
//   A strict eval. Corresponds to a VarEnvironmentObject, where its var
//   bindings lives.
//
// Eval
//   A sloppy eval. This is an empty scope, used only in the frontend, to
//   detect redeclaration errors. It has no Environment. Any `var`s declared
//   in the eval code are bound on the nearest enclosing var environment.
//
class EvalScope : public Scope {
  friend class Scope;
  friend class AbstractBindingIter<JSAtom>;
  friend class GCMarker;
  friend class frontend::ScopeStencil;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // All bindings in an eval script are 'var' bindings. The implicit
    // lexical scope around the eval is present regardless of strictness
    // and is its own LexicalScope.
    // `vars` includes top-level functions which is distinguished by a bit
    // on the BindingName.
    //
    //            vars - [0, length)
    uint32_t length = 0;
  };

  struct alignas(ScopeDataAlignBytes) RuntimeData
      : public AbstractBaseScopeData<JSAtom> {
    SlotInfo slotInfo;
    AbstractTrailingNamesArray<JSAtom> trailingNames;

    explicit RuntimeData(size_t nameCount) : trailingNames(nameCount) {}
    RuntimeData() = delete;

    void trace(JSTracer* trc);
  };

  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  template <XDRMode mode>
  static XDRResult XDR(XDRState<mode>* xdr, ScopeKind kind,
                       HandleScope enclosing, MutableHandleScope scope);

 private:
  static EvalScope* createWithData(JSContext* cx, ScopeKind kind,
                                   MutableHandle<UniquePtr<RuntimeData>> data,
                                   HandleScope enclosing);

  template <typename AtomT, typename ShapeT>
  static bool prepareForScopeCreation(
      JSContext* cx, ScopeKind scopeKind,
      typename MaybeRootedScopeData<EvalScope, AtomT>::MutableHandleType data,
      ShapeT envShape);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  // Starting a scope, the nearest var scope that a direct eval can
  // introduce vars on.
  static Scope* nearestVarScopeForDirectEval(Scope* scope);

  uint32_t nextFrameSlot() const { return data().slotInfo.nextFrameSlot; }

  bool strict() const { return kind() == ScopeKind::StrictEval; }

  bool hasBindings() const { return data().slotInfo.length > 0; }

  bool isNonGlobal() const {
    if (strict()) {
      return true;
    }
    return !nearestVarScopeForDirectEval(enclosing())->is<GlobalScope>();
  }
};

template <>
inline bool Scope::is<EvalScope>() const {
  return kind_ == ScopeKind::Eval || kind_ == ScopeKind::StrictEval;
}

//
// Scope corresponding to the toplevel script in an ES module.
//
// Like GlobalScopes, these scopes contain both vars and lexical bindings, as
// the treating of imports and exports requires putting them in one scope.
//
// Corresponds to a ModuleEnvironmentObject on the environment chain.
//
class ModuleScope : public Scope {
  friend class GCMarker;
  friend class AbstractBindingIter<JSAtom>;
  friend class Scope;
  friend class AbstractScopePtr;
  friend class frontend::ScopeStencil;
  static const ScopeKind classScopeKind_ = ScopeKind::Module;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // Bindings are sorted by kind.
    //
    // imports - [0, varStart)
    //    vars - [varStart, letStart)
    //    lets - [letStart, constStart)
    //  consts - [constStart, length)
    uint32_t varStart = 0;
    uint32_t letStart = 0;
    uint32_t constStart = 0;
    uint32_t length = 0;
  };

  struct alignas(ScopeDataAlignBytes) RuntimeData
      : public AbstractBaseScopeData<JSAtom> {
    // The module of the scope.
    HeapPtr<ModuleObject*> module = {};
    SlotInfo slotInfo;
    AbstractTrailingNamesArray<JSAtom> trailingNames;

    explicit RuntimeData(size_t nameCount);
    RuntimeData() = delete;

    void trace(JSTracer* trc);
  };

  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  template <XDRMode mode>
  static XDRResult XDR(XDRState<mode>* xdr, HandleModuleObject module,
                       HandleScope enclosing, MutableHandleScope scope);

 private:
  static ModuleScope* createWithData(JSContext* cx,
                                     MutableHandle<UniquePtr<RuntimeData>> data,
                                     Handle<ModuleObject*> module,
                                     HandleScope enclosing);
  template <typename AtomT, typename ShapeT>
  static bool prepareForScopeCreation(
      JSContext* cx,
      typename MaybeRootedScopeData<ModuleScope, AtomT>::MutableHandleType data,
      HandleModuleObject module, ShapeT envShape);

  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  uint32_t nextFrameSlot() const { return data().slotInfo.nextFrameSlot; }

  ModuleObject* module() const { return data().module; }
  void initModule(ModuleObject* mod) { return data().module.init(mod); }

  // Off-thread compilation needs to calculate environmentChainLength for
  // an emptyGlobalScope where the global may not be available.
  static const size_t EnclosingEnvironmentChainLength = 1;
};

class WasmInstanceScope : public Scope {
  friend class AbstractBindingIter<JSAtom>;
  friend class Scope;
  friend class GCMarker;
  friend class AbstractScopePtr;
  static const ScopeKind classScopeKind_ = ScopeKind::WasmInstance;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // Bindings list the WASM memories and globals.
    //
    // memories - [0, globalsStart)
    //  globals - [globalsStart, length)
    uint32_t globalsStart = 0;
    uint32_t length = 0;
  };

  struct alignas(ScopeDataAlignBytes) RuntimeData
      : public AbstractBaseScopeData<JSAtom> {
    // The wasm instance of the scope.
    HeapPtr<WasmInstanceObject*> instance = {};
    SlotInfo slotInfo;
    AbstractTrailingNamesArray<JSAtom> trailingNames;

    explicit RuntimeData(size_t nameCount);
    RuntimeData() = delete;

    void trace(JSTracer* trc);
  };

  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  static WasmInstanceScope* create(JSContext* cx, WasmInstanceObject* instance);

 private:
  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }

 public:
  WasmInstanceObject* instance() const { return data().instance; }

  uint32_t memoriesStart() const { return 0; }

  uint32_t globalsStart() const { return data().slotInfo.globalsStart; }

  uint32_t namesCount() const { return data().slotInfo.length; }
};

// Scope corresponding to the wasm function. A WasmFunctionScope is used by
// Debugger only, and not for wasm execution.
//
class WasmFunctionScope : public Scope {
  friend class AbstractBindingIter<JSAtom>;
  friend class Scope;
  friend class GCMarker;
  friend class AbstractScopePtr;
  static const ScopeKind classScopeKind_ = ScopeKind::WasmFunction;

 public:
  struct SlotInfo {
    // Frame slots [0, nextFrameSlot) are live when this is the innermost
    // scope.
    uint32_t nextFrameSlot = 0;

    // Bindings are the local variable names.
    //
    //    vars - [0, length)
    uint32_t length = 0;
  };

  struct alignas(ScopeDataAlignBytes) RuntimeData
      : public AbstractBaseScopeData<JSAtom> {
    SlotInfo slotInfo;
    AbstractTrailingNamesArray<JSAtom> trailingNames;

    explicit RuntimeData(size_t nameCount) : trailingNames(nameCount) {}
    RuntimeData() = delete;

    void trace(JSTracer* trc);
  };

  using ParserData = ParserScopeData<SlotInfo>;

  template <typename NameT>
  using AbstractData =
      typename std::conditional_t<std::is_same<NameT, JSAtom>::value,
                                  RuntimeData, ParserData>;

  static WasmFunctionScope* create(JSContext* cx, HandleScope enclosing,
                                   uint32_t funcIndex);

 private:
  RuntimeData& data() { return *static_cast<RuntimeData*>(rawData()); }

  const RuntimeData& data() const {
    return *static_cast<const RuntimeData*>(rawData());
  }
};

template <typename F>
void Scope::applyScopeDataTyped(F&& f) {
  switch (kind()) {
    case ScopeKind::Function: {
      f(&as<FunctionScope>().data());
      break;
      case ScopeKind::FunctionBodyVar:
        f(&as<VarScope>().data());
        break;
      case ScopeKind::Lexical:
      case ScopeKind::SimpleCatch:
      case ScopeKind::Catch:
      case ScopeKind::NamedLambda:
      case ScopeKind::StrictNamedLambda:
      case ScopeKind::FunctionLexical:
      case ScopeKind::ClassBody:
        f(&as<LexicalScope>().data());
        break;
      case ScopeKind::With:
        // With scopes do not have data.
        break;
      case ScopeKind::Eval:
      case ScopeKind::StrictEval:
        f(&as<EvalScope>().data());
        break;
      case ScopeKind::Global:
      case ScopeKind::NonSyntactic:
        f(&as<GlobalScope>().data());
        break;
      case ScopeKind::Module:
        f(&as<ModuleScope>().data());
        break;
      case ScopeKind::WasmInstance:
        f(&as<WasmInstanceScope>().data());
        break;
      case ScopeKind::WasmFunction:
        f(&as<WasmFunctionScope>().data());
        break;
    }
  }
}

//
// An iterator for a Scope's bindings. This is the source of truth for frame
// and environment object layout.
//
// It may be placed in GC containers; for example:
//
//   for (Rooted<BindingIter> bi(cx, BindingIter(scope)); bi; bi++) {
//     use(bi);
//     SomeMayGCOperation();
//     use(bi);
//   }
//
template <typename NameT>
class BaseAbstractBindingIter {
 protected:
  // Bindings are sorted by kind. Because different Scopes have differently
  // laid out {Runtime,Parser}Data for packing, BindingIter must handle all
  // binding kinds.
  //
  // Kind ranges:
  //
  //            imports - [0, positionalFormalStart)
  // positional formals - [positionalFormalStart, nonPositionalFormalStart)
  //      other formals - [nonPositionalParamStart, varStart)
  //               vars - [varStart, letStart)
  //               lets - [letStart, constStart)
  //             consts - [constStart, length)
  //
  // Access method when not closed over:
  //
  //            imports - name
  // positional formals - argument slot
  //      other formals - frame slot
  //               vars - frame slot
  //               lets - frame slot
  //             consts - frame slot
  //
  // Access method when closed over:
  //
  //            imports - name
  // positional formals - environment slot or name
  //      other formals - environment slot or name
  //               vars - environment slot or name
  //               lets - environment slot or name
  //             consts - environment slot or name
  MOZ_INIT_OUTSIDE_CTOR uint32_t positionalFormalStart_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t nonPositionalFormalStart_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t varStart_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t letStart_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t constStart_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t length_;

  MOZ_INIT_OUTSIDE_CTOR uint32_t index_;

  enum Flags : uint8_t {
    CannotHaveSlots = 0,
    CanHaveArgumentSlots = 1 << 0,
    CanHaveFrameSlots = 1 << 1,
    CanHaveEnvironmentSlots = 1 << 2,

    // See comment in settle below.
    HasFormalParameterExprs = 1 << 3,
    IgnoreDestructuredFormalParameters = 1 << 4,

    // Truly I hate named lambdas.
    IsNamedLambda = 1 << 5
  };

  static const uint8_t CanHaveSlotsMask = 0x7;

  MOZ_INIT_OUTSIDE_CTOR uint8_t flags_;
  MOZ_INIT_OUTSIDE_CTOR uint16_t argumentSlot_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t frameSlot_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t environmentSlot_;

  MOZ_INIT_OUTSIDE_CTOR AbstractBindingName<NameT>* names_;

  void init(uint32_t positionalFormalStart, uint32_t nonPositionalFormalStart,
            uint32_t varStart, uint32_t letStart, uint32_t constStart,
            uint8_t flags, uint32_t firstFrameSlot,
            uint32_t firstEnvironmentSlot, AbstractBindingName<NameT>* names,
            uint32_t length) {
    positionalFormalStart_ = positionalFormalStart;
    nonPositionalFormalStart_ = nonPositionalFormalStart;
    varStart_ = varStart;
    letStart_ = letStart;
    constStart_ = constStart;
    length_ = length;
    index_ = 0;
    flags_ = flags;
    argumentSlot_ = 0;
    frameSlot_ = firstFrameSlot;
    environmentSlot_ = firstEnvironmentSlot;
    names_ = names;

    settle();
  }

  void init(LexicalScope::AbstractData<NameT>& data, uint32_t firstFrameSlot,
            uint8_t flags);

  void init(FunctionScope::AbstractData<NameT>& data, uint8_t flags);

  void init(VarScope::AbstractData<NameT>& data, uint32_t firstFrameSlot);
  void init(GlobalScope::AbstractData<NameT>& data);
  void init(EvalScope::AbstractData<NameT>& data, bool strict);
  void init(ModuleScope::AbstractData<NameT>& data);
  void init(WasmInstanceScope::AbstractData<NameT>& data);
  void init(WasmFunctionScope::AbstractData<NameT>& data);

  bool hasFormalParameterExprs() const {
    return flags_ & HasFormalParameterExprs;
  }

  bool ignoreDestructuredFormalParameters() const {
    return flags_ & IgnoreDestructuredFormalParameters;
  }

  bool isNamedLambda() const { return flags_ & IsNamedLambda; }

  void increment() {
    MOZ_ASSERT(!done());
    if (flags_ & CanHaveSlotsMask) {
      if (canHaveArgumentSlots()) {
        if (index_ < nonPositionalFormalStart_) {
          MOZ_ASSERT(index_ >= positionalFormalStart_);
          argumentSlot_++;
        }
      }
      if (closedOver()) {
        // Imports must not be given known slots. They are
        // indirect bindings.
        MOZ_ASSERT(kind() != BindingKind::Import);
        MOZ_ASSERT(canHaveEnvironmentSlots());
        environmentSlot_++;
      } else if (canHaveFrameSlots()) {
        // Usually positional formal parameters don't have frame
        // slots, except when there are parameter expressions, in
        // which case they act like lets.
        if (index_ >= nonPositionalFormalStart_ ||
            (hasFormalParameterExprs() && name())) {
          frameSlot_++;
        }
      }
    }
    index_++;
  }

  void settle() {
    if (ignoreDestructuredFormalParameters()) {
      while (!done() && !name()) {
        increment();
      }
    }
  }

  BaseAbstractBindingIter() = default;

 public:
  BaseAbstractBindingIter(LexicalScope::AbstractData<NameT>& data,
                          uint32_t firstFrameSlot, bool isNamedLambda) {
    init(data, firstFrameSlot, isNamedLambda ? IsNamedLambda : 0);
  }

  BaseAbstractBindingIter(FunctionScope::AbstractData<NameT>& data,
                          bool hasParameterExprs) {
    init(data, IgnoreDestructuredFormalParameters |
                   (hasParameterExprs ? HasFormalParameterExprs : 0));
  }

  BaseAbstractBindingIter(VarScope::AbstractData<NameT>& data,
                          uint32_t firstFrameSlot) {
    init(data, firstFrameSlot);
  }

  explicit BaseAbstractBindingIter(GlobalScope::AbstractData<NameT>& data) {
    init(data);
  }

  explicit BaseAbstractBindingIter(ModuleScope::AbstractData<NameT>& data) {
    init(data);
  }

  explicit BaseAbstractBindingIter(
      WasmFunctionScope::AbstractData<NameT>& data) {
    init(data);
  }

  BaseAbstractBindingIter(EvalScope::AbstractData<NameT>& data, bool strict) {
    init(data, strict);
  }

  MOZ_IMPLICIT BaseAbstractBindingIter(
      const BaseAbstractBindingIter<NameT>& bi) = default;

  bool done() const { return index_ == length_; }

  explicit operator bool() const { return !done(); }

  void operator++(int) {
    increment();
    settle();
  }

  bool isLast() const {
    MOZ_ASSERT(!done());
    return index_ + 1 == length_;
  }

  bool canHaveArgumentSlots() const { return flags_ & CanHaveArgumentSlots; }

  bool canHaveFrameSlots() const { return flags_ & CanHaveFrameSlots; }

  bool canHaveEnvironmentSlots() const {
    return flags_ & CanHaveEnvironmentSlots;
  }

  typename AbstractBindingName<NameT>::NamePointerT name() const {
    MOZ_ASSERT(!done());
    return names_[index_].name();
  }

  bool closedOver() const {
    MOZ_ASSERT(!done());
    return names_[index_].closedOver();
  }

  BindingLocation location() const {
    MOZ_ASSERT(!done());
    if (!(flags_ & CanHaveSlotsMask)) {
      return BindingLocation::Global();
    }
    if (index_ < positionalFormalStart_) {
      return BindingLocation::Import();
    }
    if (closedOver()) {
      MOZ_ASSERT(canHaveEnvironmentSlots());
      return BindingLocation::Environment(environmentSlot_);
    }
    if (index_ < nonPositionalFormalStart_ && canHaveArgumentSlots()) {
      return BindingLocation::Argument(argumentSlot_);
    }
    if (canHaveFrameSlots()) {
      return BindingLocation::Frame(frameSlot_);
    }
    MOZ_ASSERT(isNamedLambda());
    return BindingLocation::NamedLambdaCallee();
  }

  BindingKind kind() const {
    MOZ_ASSERT(!done());
    if (index_ < positionalFormalStart_) {
      return BindingKind::Import;
    }
    if (index_ < varStart_) {
      // When the parameter list has expressions, the parameters act
      // like lexical bindings and have TDZ.
      if (hasFormalParameterExprs()) {
        return BindingKind::Let;
      }
      return BindingKind::FormalParameter;
    }
    if (index_ < letStart_) {
      return BindingKind::Var;
    }
    if (index_ < constStart_) {
      return BindingKind::Let;
    }
    if (isNamedLambda()) {
      return BindingKind::NamedLambdaCallee;
    }
    return BindingKind::Const;
  }

  bool isTopLevelFunction() const {
    MOZ_ASSERT(!done());
    bool result = names_[index_].isTopLevelFunction();
    MOZ_ASSERT_IF(result, kind() == BindingKind::Var);
    return result;
  }

  bool hasArgumentSlot() const {
    MOZ_ASSERT(!done());
    if (hasFormalParameterExprs()) {
      return false;
    }
    return index_ >= positionalFormalStart_ &&
           index_ < nonPositionalFormalStart_;
  }

  uint16_t argumentSlot() const {
    MOZ_ASSERT(canHaveArgumentSlots());
    return mozilla::AssertedCast<uint16_t>(index_);
  }

  uint32_t nextFrameSlot() const {
    MOZ_ASSERT(canHaveFrameSlots());
    return frameSlot_;
  }

  uint32_t nextEnvironmentSlot() const {
    MOZ_ASSERT(canHaveEnvironmentSlots());
    return environmentSlot_;
  }
};

template <typename NameT>
class AbstractBindingIter;

template <>
class AbstractBindingIter<JSAtom> : public BaseAbstractBindingIter<JSAtom> {
  using Base = BaseAbstractBindingIter<JSAtom>;

 public:
  AbstractBindingIter<JSAtom>(ScopeKind kind, BaseScopeData* data,
                              uint32_t firstFrameSlot);

  explicit AbstractBindingIter<JSAtom>(Scope* scope);
  explicit AbstractBindingIter<JSAtom>(JSScript* script);

  using Base::Base;

  void trace(JSTracer* trc);
};

template <>
class AbstractBindingIter<frontend::TaggedParserAtomIndex>
    : public BaseAbstractBindingIter<frontend::TaggedParserAtomIndex> {
  using Base = BaseAbstractBindingIter<frontend::TaggedParserAtomIndex>;

 public:
  using Base::Base;
};

void DumpBindings(JSContext* cx, Scope* scope);
JSAtom* FrameSlotName(JSScript* script, jsbytecode* pc);

Shape* EmptyEnvironmentShape(JSContext* cx, const JSClass* cls,
                             uint32_t numSlots, uint32_t baseShapeFlags);

template <class T>
Shape* EmptyEnvironmentShape(JSContext* cx) {
  return EmptyEnvironmentShape(cx, &T::class_, T::RESERVED_SLOTS,
                               T::BASESHAPE_FLAGS);
}

//
// A refinement BindingIter that only iterates over positional formal
// parameters of a function.
//
class PositionalFormalParameterIter : public BindingIter {
  void settle() {
    if (index_ >= nonPositionalFormalStart_) {
      index_ = length_;
    }
  }

 public:
  explicit PositionalFormalParameterIter(Scope* scope);
  explicit PositionalFormalParameterIter(JSScript* script);

  void operator++(int) {
    BindingIter::operator++(1);
    settle();
  }

  bool isDestructured() const { return !name(); }
};

//
// Iterator for walking the scope chain.
//
// It may be placed in GC containers; for example:
//
//   for (Rooted<ScopeIter> si(cx, ScopeIter(scope)); si; si++) {
//     use(si);
//     SomeMayGCOperation();
//     use(si);
//   }
//
class MOZ_STACK_CLASS ScopeIter {
  Scope* scope_;

 public:
  explicit ScopeIter(Scope* scope) : scope_(scope) {}

  explicit ScopeIter(JSScript* script);

  explicit ScopeIter(const ScopeIter& si) = default;

  bool done() const { return !scope_; }

  explicit operator bool() const { return !done(); }

  void operator++(int) {
    MOZ_ASSERT(!done());
    scope_ = scope_->enclosing();
  }

  Scope* scope() const {
    MOZ_ASSERT(!done());
    return scope_;
  }

  ScopeKind kind() const {
    MOZ_ASSERT(!done());
    return scope_->kind();
  }

  // Returns the shape of the environment if it is known. It is possible to
  // hasSyntacticEnvironment and to have no known shape, e.g., eval.
  Shape* environmentShape() const { return scope()->environmentShape(); }

  // Returns whether this scope has a syntactic environment (i.e., an
  // Environment that isn't a non-syntactic With or NonSyntacticVariables)
  // on the environment chain.
  bool hasSyntacticEnvironment() const;

  void trace(JSTracer* trc) {
    if (scope_) {
      TraceRoot(trc, &scope_, "scope iter scope");
    }
  }
};

//
// Specializations of Rooted containers for the iterators.
//

template <typename Wrapper>
class WrappedPtrOperations<BindingIter, Wrapper> {
  const BindingIter& iter() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  bool done() const { return iter().done(); }
  explicit operator bool() const { return !done(); }
  bool isLast() const { return iter().isLast(); }
  bool canHaveArgumentSlots() const { return iter().canHaveArgumentSlots(); }
  bool canHaveFrameSlots() const { return iter().canHaveFrameSlots(); }
  bool canHaveEnvironmentSlots() const {
    return iter().canHaveEnvironmentSlots();
  }
  JSAtom* name() const { return iter().name(); }
  bool closedOver() const { return iter().closedOver(); }
  BindingLocation location() const { return iter().location(); }
  BindingKind kind() const { return iter().kind(); }
  bool isTopLevelFunction() const { return iter().isTopLevelFunction(); }
  bool hasArgumentSlot() const { return iter().hasArgumentSlot(); }
  uint16_t argumentSlot() const { return iter().argumentSlot(); }
  uint32_t nextFrameSlot() const { return iter().nextFrameSlot(); }
  uint32_t nextEnvironmentSlot() const { return iter().nextEnvironmentSlot(); }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<BindingIter, Wrapper>
    : public WrappedPtrOperations<BindingIter, Wrapper> {
  BindingIter& iter() { return static_cast<Wrapper*>(this)->get(); }

 public:
  void operator++(int) { iter().operator++(1); }
};

template <typename Wrapper>
class WrappedPtrOperations<ScopeIter, Wrapper> {
  const ScopeIter& iter() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  bool done() const { return iter().done(); }
  explicit operator bool() const { return !done(); }
  Scope* scope() const { return iter().scope(); }
  ScopeKind kind() const { return iter().kind(); }
  Shape* environmentShape() const { return iter().environmentShape(); }
  bool hasSyntacticEnvironment() const {
    return iter().hasSyntacticEnvironment();
  }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<ScopeIter, Wrapper>
    : public WrappedPtrOperations<ScopeIter, Wrapper> {
  ScopeIter& iter() { return static_cast<Wrapper*>(this)->get(); }

 public:
  void operator++(int) { iter().operator++(1); }
};

Shape* CreateEnvironmentShape(JSContext* cx, BindingIter& bi,
                              const JSClass* cls, uint32_t numSlots,
                              uint32_t baseShapeFlags);

Shape* CreateEnvironmentShape(
    JSContext* cx, frontend::CompilationAtomCache& atomCache,
    AbstractBindingIter<frontend::TaggedParserAtomIndex>& bi,
    const JSClass* cls, uint32_t numSlots, uint32_t baseShapeFlags);

Shape* EmptyEnvironmentShape(JSContext* cx, const JSClass* cls,
                             uint32_t numSlots, uint32_t baseShapeFlags);

}  // namespace js

namespace JS {

template <>
struct GCPolicy<js::ScopeKind> : public IgnoreGCPolicy<js::ScopeKind> {};

template <typename T>
struct ScopeDataGCPolicy : public NonGCPointerPolicy<T> {};

#define DEFINE_SCOPE_DATA_GCPOLICY(Data)              \
  template <>                                         \
  struct MapTypeToRootKind<Data*> {                   \
    static const RootKind kind = RootKind::Traceable; \
  };                                                  \
  template <>                                         \
  struct GCPolicy<Data*> : public ScopeDataGCPolicy<Data*> {}

DEFINE_SCOPE_DATA_GCPOLICY(js::LexicalScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::FunctionScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::VarScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::GlobalScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::EvalScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::ModuleScope::RuntimeData);
DEFINE_SCOPE_DATA_GCPOLICY(js::WasmFunctionScope::RuntimeData);

#undef DEFINE_SCOPE_DATA_GCPOLICY

namespace ubi {

template <>
class Concrete<js::Scope> : TracerConcrete<js::Scope> {
 protected:
  explicit Concrete(js::Scope* ptr) : TracerConcrete<js::Scope>(ptr) {}

 public:
  static void construct(void* storage, js::Scope* ptr) {
    new (storage) Concrete(ptr);
  }

  CoarseType coarseType() const final { return CoarseType::Script; }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

}  // namespace ubi
}  // namespace JS

#endif  // vm_Scope_h

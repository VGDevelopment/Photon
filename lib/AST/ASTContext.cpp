#include "photon/AST/ASTContext.h"
#include "ForeignRepresentationInfo.h"
#include "photon/AST/ConcreteDeclRef.h"
#include "photon/AST/DiagnosticEngine.h"
#include "photon/AST/DiagnosticsSema.h"
#include "photon/AST/ExistentialLayout.h"
#include "photon/AST/ForeignErrorConvention.h"
#include "photon/AST/GenericEnvironment.h"
#include "photon/AST/GenericSignatureBuilder.h"
#include "photon/AST/KnownProtocols.h"
#include "photon/AST/LazyResolver.h"
#include "photon/AST/ModuleLoader.h"
#include "photon/AST/NameLookup.h"
#include "photon/AST/ParameterList.h"
#include "photon/AST/ProtocolConformance.h"
#include "photon/AST/RawComment.h"
#include "photon/AST/SILLayout.h"
#include "photon/AST/TypeCheckerDebugConsumer.h"
#include "photon/Basic/Compiler.h"
#include "photon/Basic/SourceManager.h"
#include "photon/Basic/Statistic.h"
#include "photon/Basic/StringExtras.h"
#include "photon/Parse/Lexer.h" 
#include "photon/Strings.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Compiler.h"
#include <algorithm>
#include <memory>

using namespace photon;
    
#define DEBUG_TYPE "ASTContext"
    
STATISTIC(NumRegisteredGenericSignatureBuilders,
      "# of succesfully registered signature builders");
STATISTIC(NumRegisteredGenericSignatureBuildersAlready,
      "# of succesfully already registered signature builders");
#define PHOTON_GSB_EXPENSIVE_ASSERTIONS 0
LazyResolver::~LazyResolver() = default;
DelegatingLazyResolver::~DelegatingLazyResolver() = default;
void ModuleLoader::anchor() {
        
}
    
void ClangModuleLoader::anchor() {
        
}
    
llvm::StringRef photon::getProtocolName(KnownProtocolKind kind) {
    switch (kind) {
        #define PROTOCOL_WITH_NAME(Id, Name) \
        case KnownProtocolKind::Id: \
            return Name;
        #include "photon/AST/KnownProtocols.def"
    }
    llvm_unreachable("bad KnownProtocolKind");
}
    
namespace {
    
    typedef std::tuple<ClassDecl *, ObjCSelector, bool> ObjCMethodConflict;
    typedef std::pair<DeclContext *, AbstractFunctionDecl *> ObjCUnsatisfiedOptReq;
    enum class SearchPathKind : uint8_t {
        Import = 1 << 0,
        Framework = 1 << 1
    }
    
} // end the without name namespace

using AssociativityCacheType = llvm::DenseMap<std::pair<PrecedenceGroupDecl *, PrecedenceGroupDecl *>, Associativity>;

#define FOR_KNOWN_FOUNDATION_TYPES(MACRO) \
    MACRO(NSError) \
    MACRO(NSNumber) \
    MACRO(NSValue)
struct ASTContext::Implementation {
    Implementation();
    ~Implementation();
    llvm::BumpPtrAllocator Allocator; // used later on
    std::vector<std::function<void(void)>> Cleanups;
    LazyResolver *Resolver = nullptr;
    llvm::StringMap<char, llvm::BumpPtrAllocator&> IdentifierTable;
    PrecedenceGroupDecl *AssignmentPrecedence = nullptr;
    PrecedenceGroupDecl *CastingPrecedence = nullptr;
    PrecedenceGroupDecl *FunctionArrowPrecedence = nullptr;
    PrecedenceGroupDecl *TernaryPrecedence = nullptr;
    PrecedenceGroupDecl *DefaultPrecedence = nullptr;
    CanType AnyObjectType;
    #define KNOWN_STDLIB_TYPE_DECL(NAME, DECL_CLASS, NUM_GENERIC_PARAMS) \
    DECL_CLASS *NAME##Decl = nullptr;
    #include "photon/AST/KnownStdlibTypes.def"
    FuncDecl *PlusFunctionOnRangeReplaceableCollection = nullptr;
    FuncDecl *PlusFunctionOnString = nullptr;
    EnumElementDecl *OptionalSomeDecl = nullptr;
    EnumElementDecl *OptionalNoneDecl = nullptr;
    EnumElementDecl *ImplicitlyUnwrappedOptionalSomeDecl = nullptr;
    EnumElementDecl *ImplicitlyUnwrappedOptionalNoneDecl = nullptr;
    VarDecl *UnsafeMutableRawPointerMemoryDecl = nullptr;
    VarDecl *UnsafeRawPointerMemoryDecl = nullptr;
    VarDecl *UnsafeMutablePointerMemoryDecl = nullptr;
    VarDecl *UnsafePointerMemoryDecl = nullptr;
    VarDecl *AutoreleasingUnsafeMutablePointerMemoryDecl = nullptr;
    TypeAliasDecl *VoidDecl = nullptr;
    StructDecl *ObjCBoolDecl = nullptr;
    #define CACHE_FOUNDATION_DECL(NAME) \
    ClassDecl *NAME##Decl = nullptr;
    FOR_KNOWN_FOUNDATION_TYPES(CACHE_FOUNDATION_DECL)
    #undef CACHE_FOUNDATION_DECL
    #define FUNC_DECL(Name, Id) FuncDecl *Get##Name = nullptr;
    #include "photon/AST/KnownDecls.def"
    FuncDecl *GetBoolDecl = nullptr;
    FuncDecl *EqualIntDecl = nullptr;
    FuncDecl *MixForSynthesizedHashValueDecl = nullptr;
    FuncDecl *MixIntDecl = nullptr;
    FuncDecl *ArrayAppendElementDecl = nullptr;
    FuncDecl *ArrayReserveCapacityDecl = nullptr;
    FuncDecl *UnimplementedInitializerDecl = nullptr;
    FuncDecl *UndefinedDecl = nullptr;
    FuncDecl *IsOSVersionAtLeastDecl = nullptr;
    ProtocolDecl *KnownProtocols[NumKnownProtocols] = { };
    SmallVector<std::unique_ptr<photon::ModuleLoader>, 4> ModuleLoaders;
    ClangModuleLoader *TheClangModuleLoader = nullptr;
    llvm::DenseMap<const Decl *, RawComment> RawComments;
    llvm::DenseMap<const Decl *, StringRef> BriefComments;
    llvm::DenseMap<const ValueDecl *, unsigned> LocalDiscriminators;
    llvm::DenseMap<const AbstractFunctionDecl *, ForeignErrorConvention> ForeignErrorConventions;
    AssociativityCacheType AssociativityCache;
    llvm::DenseMap<NormalProtocolConformance *, std::vector<ASTContext::DelayedConformanceDiag>> DelayedConformanceDiags;
    llvm::DenseMap<NormalProtocolConformance *, std::vector<ValueDecl*>> DelayedMissingWitnesses;
    llvm::DenseMap<const DeclContext *, LazyContextData *> LazyContexts;
    llvm::DenseMap<GenericSignature *, std::unique_ptr<GenericSignatureBuilder>> GenericSignatureBuilders;
    llvm::DenseMap<GenericSignatureBuilder *, GenericEnvironment *> CanonicalGenericEnvironments;
    llvm::DenseMap<std::pair<const ClassDecl *, char>, std::unique_ptr<InheritedNameSet>> AllProperties;
    llvm::DenseMap<std::pair<const clang::ObjCInterfaceDecl *, char>, std::unique_ptr<InheritedNameSet>> AllPropertiesObjC;
    CanGenericSignature SingleGenericParameterSignature;
    llvm::DenseMap<CanType, CanGenericSignature> ExistentialSignatures;
    llvm::DenseMap<const AssociatedTypeDecl *, ArrayRef<AssociatedTypeDecl *>> AssociatedTypeOverrides;
    
    struct Arena {
        llvm::DenseMap<Type, ErrorType *> ErrorTypesWithOriginal;
        llvm::FoldingSet<TupleType> TupleTypes;
        llvm::DenseMap<std::pair<Type,char>, MetatypeType*> MetatypeTypes;
        llvm::DenseMap<std::pair<Type,char>, ExistentialMetatypeType*> ExistentialMetatypeTypes;
        llvm::DenseMap<std::pair<Type,std::pair<Type,unsigned>>, FunctionType*> FunctionTypes;
        llvm::DenseMap<Type, ArraySliceType*> ArraySliceTypes;
        llvm::DenseMap<std::pair<Type, Type>, DictionaryType *> DictionaryTypes;
        llvm::DenseMap<Type, OptionalType*> OptionalTypes;
        llvm::DenseMap<Type, ImplicitlyUnwrappedOptionalType*> ImplicitlyUnwrappedOptionalTypes;
        llvm::DenseMap<std::pair<Type, unsigned>, ParenType*> ParenTypes;
        llvm::DenseMap<uintptr_t, ReferenceStorageType*> ReferenceStorageTypes;
        llvm::DenseMap<Type, LValueType*> LValueTypes;
        llvm::DenseMap<Type, InOutType*> InOutTypes;
        llvm::DenseMap<std::pair<Type, void*>, DependentMemberType *> DependentMemberTypes;
        llvm::DenseMap<Type, DynamicSelfType *> DynamicSelfTypes;
        llvm::FoldingSet<EnumType> EnumTypes;
        llvm::FoldingSet<StructType> StructTypes;
        llvm::FoldingSet<ClassType> ClassTypes;
        llvm::FoldingSet<UnboundGenericType> UnboundGenericTypes;
        llvm::FoldingSet<BoundGenericType> BoundGenericTypes;
        llvm::FoldingSet<ProtocolType> ProtocolTypes;
        llvm::FoldingSet<ProtocolCompositionType> ProtocolCompositionTypes;
        llvm::FoldingSet<LayoutConstraintInfo> LayoutConstraints;
        llvm::FoldingSet<NormalProtocolConformance> NormalConformances;
        llvm::FoldingSet<SpecializedProtocolConformance> SpecializedConformances;
        llvm::FoldingSet<InheritedProtocolConformance> InheritedConformances;
        ~Arena() {
            for (auto &conformance : SpecializedConformances) conformance.~SpecializedProtocolConformance();
            #if PHOTON_COMPILER_IS_MSVC
            #pragma warning (disable: 4189)
            #endif
            for (auto &conformance : InheritedConformances) conformance.~InheritedProtocolConformance();
            #if PHOTON_COMPILER_IS_MSVC
            #pragma warning (default: 4189)
            #endif
            for (auto &conformance : NormalConformances) conformance.~NormalProtocolConformance();
        }
        size_t getTotalMemory() const;
    };
    
    llvm::DenseMap<ModuleDecl*, ModuleType*> ModuleTypes;
    llvm::DenseMap<std::pair<unsigned, unsigned>, GenericTypeParamType *> GenericParamTypes;
    llvm::FoldingSet<GenericFunctionType> GenericFunctionTypes;
    llvm::FoldingSet<SILFunctionType> SILFunctionTypes;
    llvm::DenseMap<CanType, SILBlockStorageType *> SILBlockStorageTypes;
    llvm::FoldingSet<SILBoxType> SILBoxTypes;
    llvm::DenseMap<BuiltinIntegerWidth, BuiltinIntegerType*> IntegerTypes;
    llvm::FoldingSet<BuiltinVectorType> BuiltinVectorTypes;
    llvm::FoldingSet<GenericSignature> GenericSignatures;
    llvm::FoldingSet<DeclName::CompoundDeclName> CompoundNames;
    llvm::DenseMap<UUID, ArchetypeType *> OpenedExistentialArchetypes;
    
}
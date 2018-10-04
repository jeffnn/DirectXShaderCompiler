///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// DxilShaderAccessTracking.cpp                                        //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
// Provides a pass to add instrumentation to determine pixel hit count and   //
// cost. Used by PIX.                                                        //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/HLSL/DxilGenerationPass.h"
#include "dxc/HLSL/DxilOperations.h"
#include "dxc/HLSL/DxilInstructions.h"
#include "dxc/HLSL/DxilModule.h"
#include "dxc/HLSL/DxilPIXPasses.h"
#include "dxc/HLSL/DxilSpanAllocator.h"

#include "llvm/IR/PassManager.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Transforms/Utils/Local.h"
#include <deque>

#ifdef _WIN32
#include <winerror.h>
#endif

using namespace llvm;
using namespace hlsl;


void ThrowIf(bool a)
{
  if (a) {
    throw ::hlsl::Exception(E_INVALIDARG);
  }
}

//---------------------------------------------------------------------------------------------------------------------------------
// These types are taken from PIX's ShaderAccessHelpers.h

enum class ShaderAccessFlags : uint32_t
{
  None = 0,
  Read = 1 << 0,
  Write = 1 << 1,

  // "Counter" access is only applicable to UAVs; it means the counter buffer attached to the UAV
  // was accessed, but not necessarily the UAV resource.
  Counter = 1 << 2
};

// This enum doesn't have to match PIX's version, because the values are received from PIX encoded in ASCII.
// However, for ease of comparing this code with PIX, and to be less confusing to future maintainers, this
// enum does indeed match the same-named enum in PIX.
enum class RegisterType
{
  CBV,
  SRV,
  UAV,
  RTV, // not used. 
  DSV, // not used. 
  Sampler,
  SOV, // not used.
  Invalid,
  Terminator
};

RegisterType RegisterTypeFromResourceClass(DXIL::ResourceClass c) {
  switch (c)
  {
  case DXIL::ResourceClass::SRV    : return RegisterType::SRV    ; break;
  case DXIL::ResourceClass::UAV    : return RegisterType::UAV    ; break;
  case DXIL::ResourceClass::CBuffer: return RegisterType::CBV    ; break;
  case DXIL::ResourceClass::Sampler: return RegisterType::Sampler; break;
  case DXIL::ResourceClass::Invalid: return RegisterType::Invalid; break;
  default:
    ThrowIf(true);
    return RegisterType::Invalid;
  }
}

struct RegisterTypeAndSpace
{
  bool operator < (const RegisterTypeAndSpace & o) const {
    return static_cast<int>(Type) < static_cast<int>(o.Type) ||
      (static_cast<int>(Type) == static_cast<int>(o.Type) && Space < o.Space);
  }
  RegisterType Type;
  unsigned     Space;
};

// Identifies a bind point as defined by the root signature
struct RSRegisterIdentifier
{
  RegisterType Type;
  unsigned     Space;
  unsigned     Index;

  bool operator < (const RSRegisterIdentifier & o) const {
    return
      static_cast<unsigned>(Type) < static_cast<unsigned>(o.Type) &&
      Space < o.Space &&
      Index < o.Index;
  }
};

struct SlotRange
{
  unsigned startSlot;
  unsigned numSlots;

  // Number of slots needed if no descriptors from unbounded ranges are included
  unsigned numInvariableSlots;
};


struct DxilResourceAndClass {
  DxilResourceBase * resource;
  Value * index;
  DXIL::ResourceClass resClass;
};

//---------------------------------------------------------------------------------------------------------------------------------

class DxilShaderAccessTracking : public ModulePass {
public:
  static char ID; // Pass identification, replacement for typeid
  explicit DxilShaderAccessTracking() : ModulePass(ID) {}
  const char *getPassName() const override { return "DXIL shader access tracking"; }
  bool runOnModule(Module &M) override;
  void applyOptions(PassOptions O) override;

private:
  void EnsureUAVHandleCreation(DxilModule &DM, LLVMContext & Ctx);
  void EmitAccess(LLVMContext & Ctx, OP *HlslOP, IRBuilder<> &, Value *slot, ShaderAccessFlags access);
  void EmitResourceAccess(DxilModule &DM, DxilResourceAndClass &res, Instruction * instruction, OP * HlslOP, LLVMContext & Ctx, ShaderAccessFlags readWrite);

private:
  bool m_CheckForDynamicIndexing = false;
  std::map<RegisterTypeAndSpace, SlotRange> m_slotAssignments;
  CallInst *m_HandleForUAV = nullptr;
  std::set<RSRegisterIdentifier> m_DynamicallyIndexedBindPoints;
  bool m_modified = false;
};

static unsigned DeserializeInt(std::deque<char> & q) {
  unsigned i = 0;

  while(!q.empty() && isdigit(q.front()))
  {
    i *= 10;
    i += q.front() - '0';
    q.pop_front();
  }
  return i;
}

static char DequeFront(std::deque<char> & q) {
  ThrowIf(q.empty());
  auto c = q.front();
  q.pop_front();
  return c;
}

static RegisterType ParseRegisterType(std::deque<char> & q) {
  switch (DequeFront(q))
  {
  case 'C': return RegisterType::CBV;
  case 'S': return RegisterType::SRV;
  case 'U': return RegisterType::UAV;
  case 'M': return RegisterType::Sampler;
  case 'I': return RegisterType::Invalid;
  default: return RegisterType::Terminator;
  }
}

static char EncodeRegisterType(RegisterType r) {
  switch (r)
  {
  case RegisterType::CBV:     return 'C';
  case RegisterType::SRV:     return 'S';
  case RegisterType::UAV:     return 'U';
  case RegisterType::Sampler: return 'M';
  case RegisterType::Invalid: return 'I';
  }
  return '.';
}

static void ValidateDelimiter(std::deque<char> & q, char d) {
  ThrowIf(q.front() != d);
  q.pop_front();
}

void DxilShaderAccessTracking::applyOptions(PassOptions O) {
  int checkForDynamic;
  GetPassOptionInt(O, "checkForDynamicIndexing", &checkForDynamic, 0);
  m_CheckForDynamicIndexing = checkForDynamic != 0;

  StringRef configOption;
  if (GetPassOption(O, "config", &configOption)) {
    std::deque<char> config;
    config.assign(configOption.begin(), configOption.end());

    // Parse slot assignments. Compare with PIX's ShaderAccessHelpers.cpp (TrackingConfiguration::SerializedRepresentation)
    RegisterType rt = ParseRegisterType(config);
    while (rt != RegisterType::Terminator) {

      RegisterTypeAndSpace rst;
      rst.Type = rt;

      rst.Space = DeserializeInt(config);
      ValidateDelimiter(config, ':');

      SlotRange sr;
      sr.startSlot = DeserializeInt(config);
      ValidateDelimiter(config, ':');

      sr.numSlots = DeserializeInt(config);
      ValidateDelimiter(config, 'i');

      sr.numInvariableSlots = DeserializeInt(config);
      ValidateDelimiter(config, ';');

      m_slotAssignments[rst] = sr;

      rt = ParseRegisterType(config);
    }
  }
}

void DxilShaderAccessTracking::EmitAccess(LLVMContext & Ctx, OP *HlslOP, IRBuilder<> & Builder, Value * slot, ShaderAccessFlags access)
{
  // Slots are four bytes each:
  auto ByteIndex = Builder.CreateMul(slot, HlslOP->GetU32Const(4));

  // Insert the UAV increment instruction:

  Function* AtomicOpFunc = HlslOP->GetOpFunc(OP::OpCode::AtomicBinOp, Type::getInt32Ty(Ctx));
  Constant* AtomicBinOpcode = HlslOP->GetU32Const((unsigned)OP::OpCode::AtomicBinOp);
  Constant* AtomicOr = HlslOP->GetU32Const((unsigned)DXIL::AtomicBinOpCode::Or);

  Constant* AccessValue = HlslOP->GetU32Const(static_cast<unsigned>(access));
  UndefValue* UndefArg = UndefValue::get(Type::getInt32Ty(Ctx));

  (void)Builder.CreateCall(AtomicOpFunc, {
      AtomicBinOpcode,// i32, ; opcode
      m_HandleForUAV, // %dx.types.Handle, ; resource handle
      AtomicOr,       // i32, ; binary operation code : EXCHANGE, IADD, AND, OR, XOR, IMIN, IMAX, UMIN, UMAX
      ByteIndex,      // i32, ; coordinate c0: byte offset
      UndefArg,       // i32, ; coordinate c1 (unused)
      UndefArg,       // i32, ; coordinate c2 (unused)
      AccessValue     // i32) ; OR value
  }, "UAVOrResult");
}

void DxilShaderAccessTracking::EmitResourceAccess(DxilModule &DM, DxilResourceAndClass &res, Instruction * instruction, OP * HlslOP, LLVMContext & Ctx, ShaderAccessFlags readWrite) {

  RegisterTypeAndSpace typeAndSpace{ RegisterTypeFromResourceClass(res.resClass), res.resource->GetSpaceID() };

  auto slot = m_slotAssignments.find(typeAndSpace);
  // If the assignment isn't found, we assume it's not accessed
  if (slot != m_slotAssignments.end()) {

    m_modified = true;

    EnsureUAVHandleCreation(DM, Ctx);;

    IRBuilder<> Builder(instruction);
    Value * slotIndex;

    if (isa<ConstantInt>(res.index)) {
      unsigned index = cast<ConstantInt>(res.index)->getLimitedValue();
      if (index > slot->second.numSlots) {
        // out-of-range accesses are written to slot zero:
        slotIndex = HlslOP->GetU32Const(0);
      }
      else {
        slotIndex = HlslOP->GetU32Const(slot->second.startSlot + index);
      }
    }
    else {
      RSRegisterIdentifier id{ typeAndSpace.Type, typeAndSpace.Space,  res.resource->GetID() };
      m_DynamicallyIndexedBindPoints.emplace(std::move(id));


      // CompareWithSlotLimit will contain 1 if the access is out-of-bounds (both over- and and under-flow 
      // via the unsigned >= with slot count)
      auto CompareWithSlotLimit = Builder.CreateICmpUGE(res.index, HlslOP->GetU32Const(slot->second.numSlots), "CompareWithSlotLimit");
      auto CompareWithSlotLimitAsUint = Builder.CreateCast(Instruction::CastOps::ZExt, CompareWithSlotLimit, Type::getInt32Ty(Ctx), "CompareWithSlotLimitAsUint");

      // IsInBounds will therefore contain 0 if the access is out-of-bounds, and 1 otherwise.
      auto IsInBounds = Builder.CreateSub(HlslOP->GetU32Const(1), CompareWithSlotLimitAsUint, "IsInBounds");

      auto SlotOffset = Builder.CreateAdd(res.index, HlslOP->GetU32Const(slot->second.startSlot), "SlotOffset");

      // This will drive an out-of-bounds access slot down to 0
      slotIndex = Builder.CreateMul(SlotOffset, IsInBounds, "slotIndex");
    }

    EmitAccess(Ctx, HlslOP, Builder, slotIndex, readWrite);
  }
}


DxilResourceAndClass GetResourceFromHandle(Value * resHandle, DxilModule &DM) {

  DxilResourceAndClass ret{ nullptr, nullptr, DXIL::ResourceClass::Invalid };

  CallInst *handle = cast<CallInst>(resHandle);
  DxilInst_CreateHandle createHandle(handle);


  // Dynamic rangeId is not supported - skip and let validation report the
  // error.
  if (!isa<ConstantInt>(createHandle.get_rangeId()))
    return ret;

  unsigned rangeId =
    cast<ConstantInt>(createHandle.get_rangeId())->getLimitedValue();

  auto resClass = static_cast<DXIL::ResourceClass>(createHandle.get_resourceClass_val());

  switch (resClass) {
  case DXIL::ResourceClass::SRV:
    ret.resource = &DM.GetSRV(rangeId);
    break;
  case DXIL::ResourceClass::UAV:
    ret.resource = &DM.GetUAV(rangeId);
    break;
  case DXIL::ResourceClass::CBuffer:
    ret.resource = &DM.GetCBuffer(rangeId);
    break;
  case DXIL::ResourceClass::Sampler:
    ret.resource = &DM.GetSampler(rangeId);
    break;
  default:
    DXASSERT(0, "invalid res class");
    return ret;
  }

  ret.index = createHandle.get_index();
  ret.resClass = resClass;

  return ret;
}

bool DxilShaderAccessTracking::runOnModule(Module &M)
{
  // This pass adds instrumentation for shader access to resources

  DxilModule &DM = M.GetOrCreateDxilModule();
  LLVMContext & Ctx = M.getContext();
  OP *HlslOP = DM.GetOP();

  m_modified = false;

  if (m_CheckForDynamicIndexing) {

    bool FoundDynamicIndexing = false;

    auto * CreateHandleFn = HlslOP->TryGetOpFunc(DXIL::OpCode::CreateHandle, Type::getVoidTy(Ctx));
    if (CreateHandleFn != nullptr) {
      auto CreateHandleUses = CreateHandleFn->uses();
      for (auto FI = CreateHandleUses.begin(); FI != CreateHandleUses.end(); ) {
        auto & FunctionUse = *FI++;
        auto FunctionUser = FunctionUse.getUser();
        auto instruction = cast<Instruction>(FunctionUser);
        Value * index = instruction->getOperand(3);
        if (!isa<Constant>(index)) {
          FoundDynamicIndexing = true;
          break;
        }
      }
    }
    if (FoundDynamicIndexing) {
      if (OSOverride != nullptr) {
        formatted_raw_ostream FOS(*OSOverride);
        FOS << "FoundDynamicIndexing";
      }
    }
  }
  else {
    {
      if (DM.m_ShaderFlags.GetForceEarlyDepthStencil()) {
        if (OSOverride != nullptr) {
          formatted_raw_ostream FOS(*OSOverride);
          FOS << "ShouldAssumeDsvAccess";
        }
      }

      std::vector<CallInst*> callSitesToInstrument;

      for (auto &F : M.functions()) {
        Function &function = F;
        if (!function.isDeclaration() || function.isIntrinsic() || !OP::IsDxilOpFunc(&function))
          continue;
        auto FunctionUses = F.uses();
        for (auto & FunctionUser : FunctionUses) {
          CallInst * func = dyn_cast_or_null<CallInst>(FunctionUser.getUser());
          if (func != nullptr) {
            callSitesToInstrument.push_back(func);
          }
        }
      }

      for (auto instruction : callSitesToInstrument) {
        unsigned opcode = cast<ConstantInt>(instruction->getArgOperand(0))->getLimitedValue();
        DXIL::OpCode dxilOpcode = (DXIL::OpCode)opcode;
        ShaderAccessFlags access = ShaderAccessFlags::None;
        bool functionUsesSamplerAtIndex2 = false;
        switch (dxilOpcode) {
        case DXIL::OpCode::CBufferLoadLegacy     : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = false; break;
        case DXIL::OpCode::CBufferLoad           : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = false; break;
        case DXIL::OpCode::Sample                : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = true ; break;
        case DXIL::OpCode::SampleBias            : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = true ; break;
        case DXIL::OpCode::SampleLevel           : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = true ; break;
        case DXIL::OpCode::SampleGrad            : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = true ; break;
        case DXIL::OpCode::SampleCmp             : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = true ; break;
        case DXIL::OpCode::SampleCmpLevelZero    : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = true ; break;
        case DXIL::OpCode::TextureLoad           : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = false; break;
        case DXIL::OpCode::TextureStore          : access = ShaderAccessFlags::Write  ; functionUsesSamplerAtIndex2 = false; break;
        case DXIL::OpCode::TextureGather         : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = true ; break;
        case DXIL::OpCode::TextureGatherCmp      : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = false; break;
        case DXIL::OpCode::BufferLoad            : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = false; break;
        case DXIL::OpCode::RawBufferLoad         : access = ShaderAccessFlags::Read   ; functionUsesSamplerAtIndex2 = false; break;
        case DXIL::OpCode::BufferStore           : access = ShaderAccessFlags::Write  ; functionUsesSamplerAtIndex2 = false; break;
        case DXIL::OpCode::BufferUpdateCounter   : access = ShaderAccessFlags::Counter; functionUsesSamplerAtIndex2 = false; break;
        case DXIL::OpCode::AtomicBinOp           : access = ShaderAccessFlags::Write  ; functionUsesSamplerAtIndex2 = false; break;
        case DXIL::OpCode::AtomicCompareExchange : access = ShaderAccessFlags::Write  ; functionUsesSamplerAtIndex2 = false; break;
        default:
          //do nothing: no access
          break;
        }

        if (access != ShaderAccessFlags::None) {
          auto res = GetResourceFromHandle(instruction->getOperand(1), DM);

          // Don't instrument the accesses to the UAV that we just added
          if (res.resource->GetSpaceID() == (unsigned)-2) {
            continue;
          }

          EmitResourceAccess(DM, res, instruction, HlslOP, Ctx, access);

          if (functionUsesSamplerAtIndex2) {
            auto sampler = GetResourceFromHandle(instruction->getOperand(2), DM);
            EmitResourceAccess(DM, sampler, instruction, HlslOP, Ctx, ShaderAccessFlags::Read);
          }
        }
      }
    }

    if (OSOverride != nullptr) {
      formatted_raw_ostream FOS(*OSOverride);
      FOS << "DynamicallyIndexedBindPoints=";
      for (auto const & bp : m_DynamicallyIndexedBindPoints) {
        FOS << EncodeRegisterType(bp.Type) << bp.Space << ':' << bp.Index <<';';
      }
      FOS << ".";
    }
  }

  if (m_modified) {
    DM.CollectShaderFlagsForModule();
    DM.ReEmitDxilResources();
  }
  return m_modified;
}

void DxilShaderAccessTracking::EnsureUAVHandleCreation(DxilModule &DM, LLVMContext & Ctx)
{
  if (m_HandleForUAV != nullptr)
    return;

  OP *HlslOP = DM.GetOP();

  IRBuilder<> Builder(DM.GetEntryFunction()->getEntryBlock().getFirstInsertionPt());

  unsigned int UAVResourceHandle = static_cast<unsigned int>(DM.GetUAVs().size());

  // Set up a UAV with structure of a single int
  SmallVector<llvm::Type*, 1> Elements{ Type::getInt32Ty(Ctx) };
  llvm::StructType *UAVStructTy = llvm::StructType::create(Elements, "class.RWStructuredBuffer");
  std::unique_ptr<DxilResource> pUAV = llvm::make_unique<DxilResource>();
  pUAV->SetGlobalName("PIX_CountUAVName");
  pUAV->SetGlobalSymbol(UndefValue::get(UAVStructTy->getPointerTo()));
  pUAV->SetID(UAVResourceHandle);
  pUAV->SetSpaceID((unsigned int)-2); // This is the reserved-for-tools register space
  pUAV->SetSampleCount(1);
  pUAV->SetGloballyCoherent(false);
  pUAV->SetHasCounter(false);
  pUAV->SetCompType(CompType::getI32());
  pUAV->SetLowerBound(0);
  pUAV->SetRangeSize(1);
  pUAV->SetKind(DXIL::ResourceKind::RawBuffer);

  auto pAnnotation = DM.GetTypeSystem().GetStructAnnotation(UAVStructTy);
  if (pAnnotation == nullptr) {

    pAnnotation = DM.GetTypeSystem().AddStructAnnotation(UAVStructTy);
    pAnnotation->GetFieldAnnotation(0).SetCBufferOffset(0);
    pAnnotation->GetFieldAnnotation(0).SetCompType(hlsl::DXIL::ComponentType::I32);
    pAnnotation->GetFieldAnnotation(0).SetFieldName("count");
  }

  ID = DM.AddUAV(std::move(pUAV));

  assert((unsigned)ID == UAVResourceHandle);

  // Create handle for the newly-added UAV
  Function* CreateHandleOpFunc = HlslOP->GetOpFunc(DXIL::OpCode::CreateHandle, Type::getVoidTy(Ctx));
  Constant* CreateHandleOpcodeArg = HlslOP->GetU32Const((unsigned)DXIL::OpCode::CreateHandle);
  Constant* UAVArg = HlslOP->GetI8Const(static_cast<std::underlying_type<DxilResourceBase::Class>::type>(DXIL::ResourceClass::UAV));
  Constant* MetaDataArg = HlslOP->GetU32Const(ID); // position of the metadata record in the corresponding metadata list
  Constant* IndexArg = HlslOP->GetU32Const(0); // 
  Constant* FalseArg = HlslOP->GetI1Const(0); // non-uniform resource index: false
  m_HandleForUAV = Builder.CreateCall(CreateHandleOpFunc,
  { CreateHandleOpcodeArg, UAVArg, MetaDataArg, IndexArg, FalseArg }, "PIX_CountUAV_Handle");
}

char DxilShaderAccessTracking::ID = 0;

ModulePass *llvm::createDxilShaderAccessTrackingPass() {
  return new DxilShaderAccessTracking();
}

INITIALIZE_PASS(DxilShaderAccessTracking, "hlsl-dxil-pix-shader-access-instrumentation", "HLSL DXIL shader access tracking for PIX", false, false)

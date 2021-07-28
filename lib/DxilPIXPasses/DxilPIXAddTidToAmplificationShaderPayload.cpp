///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// DxilOutputColorBecomesConstant.cpp                                        //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
// Provides a pass to turn on the early-z flag                               //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/DXIL/DxilOperations.h"
#include "dxc/DXIL/DxilUtil.h"

#include "dxc/DXIL/DxilInstructions.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DxilPIXPasses/DxilPIXPasses.h"
#include "dxc/HLSL/DxilGenerationPass.h"
#include "dxc/HLSL/DxilSpanAllocator.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Transforms/Utils/Local.h"

#include "PixPassHelpers.h"

using namespace llvm;
using namespace hlsl;

class DxilPIXAddTidToAmplificationShaderPayload : public ModulePass {

public:
  static char ID; // Pass identification, replacement for typeid
  DxilPIXAddTidToAmplificationShaderPayload() : ModulePass(ID) {}
  const char *getPassName() const override { return "DXIL Add flat thread id to payload from AS to MS"; }
  bool runOnModule(Module &M) override;
private:

    void EmitInstructionsToCopyStructContents(llvm::IRBuilder<> &B,
        llvm::Value *NewStructAlloca,
        llvm::Value*OldStructAlloca);


};

bool DxilPIXAddTidToAmplificationShaderPayload::runOnModule(Module &M) {

  DxilModule &DM = M.GetOrCreateDxilModule();
  LLVMContext &Ctx = M.getContext();
  OP *HlslOP = DM.GetOP();

  Type* OriginalPayloadStructPointerType = nullptr;
  Type* OriginalPayloadStructType = nullptr;
  Type* ExpandedPayloadStructType = nullptr;
  Type* ExpandedPayloadStructPtrType = nullptr;
  llvm::Function* entryFunction = PIXPassHelpers::GetEntryFunction(DM);
  for (inst_iterator I = inst_begin(entryFunction),
                     E = inst_end(entryFunction);
       I != E; ++I) {
      if (auto* Instr = llvm::cast<Instruction>(&*I)) {
          if (hlsl::OP::IsDxilOpFuncCallInst(Instr, hlsl::OP::OpCode::DispatchMesh))
          {
              DxilInst_DispatchMesh DispatchMesh(Instr);
              OriginalPayloadStructPointerType = DispatchMesh.get_payload()->getType();
              OriginalPayloadStructType = OriginalPayloadStructPointerType->getPointerElementType();
              //auto* PayloadTypeAsStruct = llvm::cast<llvm::StructType>(OriginalPayloadStructType);
              SmallVector<Type*, 16> Elements;
              //for (unsigned int e = 0; e < PayloadTypeAsStruct->getNumElements();
              //    ++e) {
              //    Elements.push_back(PayloadTypeAsStruct->getElementType(e));
              //}
              Elements.push_back(OriginalPayloadStructType);
              // This adds an int32 in order to pass flat thread id to the mesh
              // shader:
              Elements.push_back(Type::getInt32Ty(Ctx));
              ExpandedPayloadStructType = StructType::create(Ctx, Elements, "PIX_AS2MS_Expanded_Type");
              ExpandedPayloadStructPtrType = ExpandedPayloadStructType->getPointerTo();
          }
      }
  }

  AllocaInst* OldStructAlloca = nullptr;
  AllocaInst* NewStructAlloca = nullptr;
    for (inst_iterator I = inst_begin(entryFunction),
                     E = inst_end(entryFunction);
       I != E; ++I) {
        auto* Inst = &*I;
      if (llvm::isa<AllocaInst>(Inst)) {
          auto* Alloca = llvm::cast<AllocaInst>(Inst);
          if (Alloca->getType() == OriginalPayloadStructPointerType)
          {
              OldStructAlloca = Alloca;
              llvm::IRBuilder<> B(Alloca->getContext());
              NewStructAlloca = B.CreateAlloca(ExpandedPayloadStructType, HlslOP->GetU32Const(1), "NewPayload");
              NewStructAlloca->setAlignment(Alloca->getAlignment());
              NewStructAlloca->insertAfter(Alloca);
              //Alloca->replaceAllUsesWith(NewStructAlloca);
              std::vector<Value*> users;
              for (auto u = Alloca->user_begin(); u != Alloca->user_end(); ++u)
              {
                  users.push_back(*u);
              }

              for (auto user : users)
              {
                  user->dump();
                  if (auto* instruction = llvm::cast<Instruction>(user))
                  {
                      instruction->dump();
                      for (unsigned int i = 0; i < instruction->getNumOperands(); ++i)
                      {
                          auto* Operand = instruction->getOperand(i);
                          if (Operand == Alloca)
                          {
                              instruction->setOperand(i, NewStructAlloca);
                          }
                      }
                      if (llvm::isa<GetElementPtrInst>(instruction))
                      {
                          auto* GEP = llvm::cast<GetElementPtrInst>(instruction);
                          GEP->setSourceElementType(ExpandedPayloadStructType);
                      }
                  }
              }

              for (auto user : Alloca->users()) {
                  user->dump();
              }

              Alloca->removeFromParent();
              delete Alloca;
              break;
            }
      }
  }

  auto F = HlslOP->GetOpFunc(DXIL::OpCode::DispatchMesh, OriginalPayloadStructPointerType);
  for (auto FI = F->user_begin(); FI != F->user_end();) {
      auto* FunctionUser = *FI++;
      auto * UserInstruction = llvm::cast<Instruction>(FunctionUser);
      DxilInst_DispatchMesh DispatchMesh(UserInstruction);

      llvm::IRBuilder<> B(Ctx);
      B.SetInsertPoint(UserInstruction);

      //auto ThreadIdFunc = BC.HlslOP->GetOpFunc(DXIL::OpCode::DispatchMesh,
      //                                         Type::getInt32Ty(BC.Ctx));
      //Constant *Opcode =
      //    BC.HlslOP->GetU32Const((unsigned)DXIL::OpCode::ThreadId);
      //auto ThreadIdX =
      //    BC.Builder.CreateCall(ThreadIdFunc, {Opcode, Zero32Arg}, "ThreadIdX");

      //B.CreateCall()

      SmallVector<Value *, 2> IndexToEmbeddedOriginal;
      IndexToEmbeddedOriginal.push_back(HlslOP->GetU32Const(0));
      IndexToEmbeddedOriginal.push_back(HlslOP->GetU32Const(0));
      //auto *PointerToContainedCopyOfOriginal = B.CreateInBoundsGEP(ExpandedPayloadStructType, NewStructAlloca, IndexToEmbeddedOriginal, "PointerToCopyOfOriginal");

      SmallVector<uint32_t, 1> Index0{0}; // , 1
      Value *Insert = B.CreateLoad(DispatchMesh.get_payload(), "ExpandedValue");

      SmallVector<uint32_t, 1> Index1{ 1 };// , 1
      auto * StoreAppendedValue = B.CreateInsertValue(
          Insert, HlslOP->GetU32Const(42), Index1);

      UserInstruction->replaceUsesOfWith(NewStructAlloca, StoreAppendedValue);

      //SmallVector<uint32_t, 1> Index0{0};
      //auto *PointerToOriginal = B.CreateInBoundsGEP(
      //    OriginalPayloadStructType, OldStructAlloca, IndexToEmbeddedOriginal,
      //    "PointerToOriginal");
      //
      //EmitInstructionsToCopyStructContents(B, PointerToContainedCopyOfOriginal, PointerToOriginal);
      //
      ////auto Original = B.CreateInBoundsGEP(OriginalPayloadStructType, DispatchMesh.get_payload(), HlslOP->GetU32Const(0), "Original");
      ////auto orginalValue = B.CreateExtractValue(DispatchMesh.get_payload(), Index0);
      //B.CreateStore(OldStructAlloca, PointerToContainedCopyOfOriginal);
      ///*auto* StoreOrignal = */B.CreateInsertValue(PointerToContainedCopyOfOriginal, DispatchMesh.get_payload(), Index0);
  }

  DM.ReEmitDxilResources();

  return true;
}

void DxilPIXAddTidToAmplificationShaderPayload::EmitInstructionsToCopyStructContents(llvm::IRBuilder<> & B,
    llvm::Value * NewStructPointer,
    llvm::Value* OldStruct) {
    //DXASSERT(NewStruct->getType() == OldStruct->getType());
    //switch (NewStruct->getType()->getTypeID())
    //{
    //case llvm::Type::TypeID::FunctionTyID:
    //case llvm::Type::TypeID::StructTyID:
    //case llvm::Type::TypeID::ArrayTyID:
    //case llvm::Type::TypeID::PointerTyID:
    //case llvm::Type::TypeID::VectorTyID:
    //default:
    //  B.CreateStore(NewStruct, OldStruct);
    //}
}

char DxilPIXAddTidToAmplificationShaderPayload::ID = 0;

ModulePass *llvm::createDxilPIXAddTidToAmplificationShaderPayloadPass() { return new DxilPIXAddTidToAmplificationShaderPayload(); }

INITIALIZE_PASS(
    DxilPIXAddTidToAmplificationShaderPayload, "hlsl-dxil-PIX-add-tid-to-as-payload",
    "HLSL DXIL Add flat thread id to payload from AS to MS",
    false, false)

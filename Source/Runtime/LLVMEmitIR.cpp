#include "LLVMJIT.h"
#include "llvm/ADT/SmallVector.h"
#include "WebAssembly/Operations.h"
#include "WebAssembly/OperatorLoggingProxy.h"

#define ENABLE_LOGGING 0
#define ENABLE_FUNCTION_ENTER_EXIT_HOOKS 0

using namespace WebAssembly;

namespace LLVMJIT
{
	// The LLVM IR for a module.
	struct EmitModuleContext
	{
		const Module& module;
		ModuleInstance* moduleInstance;

		llvm::Module* llvmModule;
		std::vector<llvm::Function*> functionDefs;
		std::vector<llvm::Constant*> importedFunctionPointers;
		std::vector<llvm::Constant*> globalPointers;
		llvm::Constant* defaultTablePointer;
		llvm::Constant* defaultTableEndOffset;
		llvm::Constant* defaultMemoryBase;
		llvm::Constant* defaultMemoryAddressMask;
		
		llvm::DIBuilder diBuilder;
		llvm::DICompileUnit* diCompileUnit;
		llvm::DIFile* diModuleScope;

		llvm::DIType* diValueTypes[(size_t)ValueType::num];

		llvm::MDNode* likelyFalseBranchWeights;
		llvm::MDNode* likelyTrueBranchWeights;

		EmitModuleContext(const Module& inModule,ModuleInstance* inModuleInstance)
		: module(inModule)
		, moduleInstance(inModuleInstance)
		, llvmModule(new llvm::Module("",context))
		, diBuilder(*llvmModule)
		{
			diCompileUnit = diBuilder.createCompileUnit(0xffff,"unknown","unknown","WAVM",true,"",0);
			diModuleScope = diBuilder.createFile("unknown","unknown");

			diValueTypes[(uintp)ValueType::invalid] = nullptr;
			diValueTypes[(uintp)ValueType::i32] = diBuilder.createBasicType("i32",32,32,llvm::dwarf::DW_ATE_signed);
			diValueTypes[(uintp)ValueType::i64] = diBuilder.createBasicType("i64",64,64,llvm::dwarf::DW_ATE_signed);
			diValueTypes[(uintp)ValueType::f32] = diBuilder.createBasicType("f32",32,32,llvm::dwarf::DW_ATE_float);
			diValueTypes[(uintp)ValueType::f64] = diBuilder.createBasicType("f64",64,64,llvm::dwarf::DW_ATE_float);
			
			auto zeroAsMetadata = llvm::ConstantAsMetadata::get(emitLiteral(int32(0)));
			auto i32MaxAsMetadata = llvm::ConstantAsMetadata::get(emitLiteral(int32(INT32_MAX)));
			likelyFalseBranchWeights = llvm::MDTuple::getDistinct(context,{llvm::MDString::get(context,"branch_weights"),zeroAsMetadata,i32MaxAsMetadata});
			likelyTrueBranchWeights = llvm::MDTuple::getDistinct(context,{llvm::MDString::get(context,"branch_weights"),i32MaxAsMetadata,zeroAsMetadata});
		}

		llvm::Module* emit();
	};

	// The context used by functions involved in JITing a single AST function.
	struct EmitFunctionContext
	{
		EmitModuleContext& moduleContext;
		const Module& module;
		const Function& function;
		const FunctionType* functionType;
		FunctionInstance* functionInstance;
		llvm::Function* llvmFunction;
		llvm::IRBuilder<> irBuilder;

		std::vector<llvm::Value*> localPointers;

		llvm::DISubprogram* diFunction;

		// Information about an in-scope control structure.
		struct ControlContext
		{
			enum class Type : uint8
			{
				function,
				block,
				ifThen,
				ifElse,
				loop
			};

			Type type;
			llvm::BasicBlock* endBlock;
			llvm::PHINode* endPHI;
			llvm::BasicBlock* elseBlock;
			ResultType resultType;
			uintp outerStackSize;
			uintp outerBranchTargetStackSize;
			bool isReachable;
			bool isElseReachable;
		};

		struct BranchTarget
		{
			ResultType argumentType;
			llvm::BasicBlock* block;
			llvm::PHINode* phi;
		};

		std::vector<ControlContext> controlStack;
		std::vector<BranchTarget> branchTargetStack;
		std::vector<llvm::Value*> stack;

		EmitFunctionContext(EmitModuleContext& inEmitModuleContext,const Module& inModule,const Function& inFunction,FunctionInstance* inFunctionInstance,llvm::Function* inLLVMFunction)
		: moduleContext(inEmitModuleContext)
		, module(inModule)
		, function(inFunction)
		, functionType(module.types[inFunction.typeIndex])
		, functionInstance(inFunctionInstance)
		, llvmFunction(inLLVMFunction)
		, irBuilder(context)
		{}

		void emit();

		// Operand stack manipulation
		llvm::Value* pop()
		{
			assert(stack.size() - (controlStack.size() ? controlStack.back().outerStackSize : 0) >= 1);
			llvm::Value* result = stack.back();
			stack.pop_back();
			return result;
		}

		void popMultiple(llvm::Value** outValues,size_t num)
		{
			assert(stack.size() - (controlStack.size() ? controlStack.back().outerStackSize : 0) >= num);
			std::copy(stack.end() - num,stack.end(),outValues);
			stack.resize(stack.size() - num);
		}

		llvm::Value* getTopValue() const
		{
			return stack.back();
		}

		void push(llvm::Value* value)
		{
			stack.push_back(value);
		}

		// Creates a PHI node for the argument of branches to a basic block.
		llvm::PHINode* createPHI(llvm::BasicBlock* basicBlock,ResultType type)
		{
			if(type == ResultType::none) { return nullptr; }
			else
			{
				auto originalBlock = irBuilder.GetInsertBlock();
				irBuilder.SetInsertPoint(basicBlock);
				auto phi = irBuilder.CreatePHI(asLLVMType(type),2);
				if(originalBlock) { irBuilder.SetInsertPoint(originalBlock); }
				return phi;
			}
		}

		// Debug logging.
		void logOperator(const std::string& operatorDescription)
		{
			if(ENABLE_LOGGING)
			{
				std::string controlStackString;
				for(uintp stackIndex = 0;stackIndex < controlStack.size();++stackIndex)
				{
					switch(controlStack[stackIndex].type)
					{
					case ControlContext::Type::function: controlStackString += "F"; break;
					case ControlContext::Type::block: controlStackString += "B"; break;
					case ControlContext::Type::ifThen: controlStackString += "T"; break;
					case ControlContext::Type::ifElse: controlStackString += "E"; break;
					case ControlContext::Type::loop: controlStackString += "L"; break;
					default: Core::unreachable();
					};
					if(!controlStack[stackIndex].isReachable) { controlStackString += "-"; }
				}

				std::string stackString;
				const uintp stackBase = controlStack.size() == 0 ? 0 : controlStack.back().outerStackSize;
				for(uintp stackIndex = 0;stackIndex < stack.size();++stackIndex)
				{
					if(stackIndex == stackBase) { stackString += "| "; }
					{
						llvm::raw_string_ostream stackTypeStream(stackString);
						stack[stackIndex]->getType()->print(stackTypeStream,true);
					}
					stackString += " ";
				}
				if(stack.size() == stackBase) { stackString += "|"; }

				Log::printf(Log::Category::debug,"%-50s %-50s %-50s\n",controlStackString.c_str(),operatorDescription.c_str(),stackString.c_str());
			}
		}
		
		// Coerces an I32 value to an I1, and vice-versa.
		llvm::Value* coerceI32ToBool(llvm::Value* i32Value)
		{
			return irBuilder.CreateICmpNE(i32Value,typedZeroConstants[(size_t)ValueType::i32]);
		}
		llvm::Value* coerceBoolToI32(llvm::Value* boolValue)
		{
			return irBuilder.CreateZExt(boolValue,llvmI32Type);
		}
		
		// Bounds checks and converts a memory operation I32 address operand to a LLVM pointer.
		llvm::Value* coerceByteIndexToPointer(llvm::Value* byteIndex,uint32 offset,llvm::Type* memoryType)
		{
			// On a 64 bit runtime, if the address is 32-bits, zext it to 64-bits.
			// This is crucial for security, as LLVM will otherwise implicitly sign extend it to 64-bits in the GEP below,
			// interpreting it as a signed offset and allowing access to memory outside the sandboxed memory range.
			// There are no 'far addresses' in a 32 bit runtime.
			llvm::Value* nativeByteIndex = sizeof(uintp) == 4 ? byteIndex : irBuilder.CreateZExt(byteIndex,llvmI64Type);
			llvm::Value* offsetByteIndex = nativeByteIndex;
			if(offset)
			{
				auto nativeOffset = sizeof(uintp) == 4 ? emitLiteral((uint32)offset) : irBuilder.CreateZExt(emitLiteral((uint32)offset),llvmI64Type);
				offsetByteIndex = irBuilder.CreateAdd(nativeByteIndex,nativeOffset);
			}

			// Mask the index to the address-space size.
			auto maskedByteIndex = irBuilder.CreateAnd(offsetByteIndex,moduleContext.defaultMemoryAddressMask);

			// Cast the pointer to the appropriate type.
			auto bytePointer = irBuilder.CreateInBoundsGEP(moduleContext.defaultMemoryBase,maskedByteIndex);
			return irBuilder.CreatePointerCast(bytePointer,memoryType->getPointerTo());
		}

		// Traps a divide-by-zero
		void trapDivideByZero(ValueType type,llvm::Value* divisor)
		{
			emitConditionalTrapIntrinsic(
				irBuilder.CreateICmpEQ(divisor,typedZeroConstants[(uintp)type]),
				"wavmIntrinsics.divideByZeroTrap",FunctionType::get(),{});
		}

		llvm::Value* getLLVMIntrinsic(const std::initializer_list<llvm::Type*>& argTypes,llvm::Intrinsic::ID id)
		{
			return llvm::Intrinsic::getDeclaration(moduleContext.llvmModule,id,llvm::ArrayRef<llvm::Type*>(argTypes.begin(),argTypes.end()));
		}
		
		// Emits a call to a WAVM intrinsic function.
		llvm::Value* emitRuntimeIntrinsic(const char* intrinsicName,const FunctionType* intrinsicType,const std::initializer_list<llvm::Value*>& args)
		{
			Object* intrinsicObject = Intrinsics::find(intrinsicName,intrinsicType);
			assert(intrinsicObject);
			FunctionInstance* intrinsicFunction = asFunction(intrinsicObject);
			assert(intrinsicFunction->type == intrinsicType);
			auto intrinsicFunctionPointer = emitLiteralPointer(intrinsicFunction->nativeFunction,asLLVMType(intrinsicType)->getPointerTo());
			return irBuilder.CreateCall(intrinsicFunctionPointer,llvm::ArrayRef<llvm::Value*>(args.begin(),args.end()));
		}

		// A helper function to emit a conditional call to a non-returning intrinsic function.
		void emitConditionalTrapIntrinsic(llvm::Value* booleanCondition,const char* intrinsicName,const FunctionType* intrinsicType,const std::initializer_list<llvm::Value*>& args)
		{
			auto trueBlock = llvm::BasicBlock::Create(context,llvm::Twine(intrinsicName) + "Trap",llvmFunction);
			auto endBlock = llvm::BasicBlock::Create(context,llvm::Twine(intrinsicName) + "Skip",llvmFunction);

			irBuilder.CreateCondBr(booleanCondition,trueBlock,endBlock,moduleContext.likelyFalseBranchWeights);

			irBuilder.SetInsertPoint(trueBlock);
			emitRuntimeIntrinsic(intrinsicName,intrinsicType,args);
			irBuilder.CreateUnreachable();

			irBuilder.SetInsertPoint(endBlock);
		}

		//
		// Misc operators
		//

		void nop(NoImm) {}
		void unknown(Opcode opcode) { Core::unreachable(); }
		void error(ErrorImm imm) { Core::unreachable(); }
		
		//
		// Control structure operators
		//
		
		void pushControlStack(
			ControlContext::Type type,
			ResultType resultType,
			llvm::BasicBlock* endBlock,
			llvm::PHINode* endPHI,
			llvm::BasicBlock* elseBlock = nullptr
			)
		{
			// The unreachable operator filtering should filter out any opcodes that call pushControlStack.
			if(controlStack.size()) { errorUnless(controlStack.back().isReachable); }

			controlStack.push_back({type,endBlock,endPHI,elseBlock,resultType,stack.size(),branchTargetStack.size(),true,true});
		}

		void pushBranchTarget(ResultType branchArgumentType,llvm::BasicBlock* branchTargetBlock,llvm::PHINode* branchTargetPHI)
		{
			branchTargetStack.push_back({branchArgumentType,branchTargetBlock,branchTargetPHI});
		}

		void beginBlock(ControlStructureImm imm)
		{
			// Create an end block+phi for the block result.
			auto endBlock = llvm::BasicBlock::Create(context,"blockEnd",llvmFunction);
			auto endPHI = createPHI(endBlock,imm.resultType);

			// Push a control context that ends at the end block/phi.
			pushControlStack(ControlContext::Type::block,imm.resultType,endBlock,endPHI);
			
			// Push a branch target for the end block/phi.
			pushBranchTarget(imm.resultType,endBlock,endPHI);
		}
		void beginLoop(ControlStructureImm imm)
		{
			// Create a loop block, and an end block+phi for the loop result.
			auto loopBodyBlock = llvm::BasicBlock::Create(context,"loopBody",llvmFunction);
			auto endBlock = llvm::BasicBlock::Create(context,"loopEnd",llvmFunction);
			auto endPHI = createPHI(endBlock,imm.resultType);
			
			// Branch to the loop body and switch the IR builder to emit there.
			irBuilder.CreateBr(loopBodyBlock);
			irBuilder.SetInsertPoint(loopBodyBlock);

			// Push a control context that ends at the end block/phi.
			pushControlStack(ControlContext::Type::loop,imm.resultType,endBlock,endPHI);
			
			// Push a branch target for the loop body start.
			pushBranchTarget(ResultType::none,loopBodyBlock,nullptr);
		}
		void beginIf(ControlStructureImm imm)
		{
			// Create a then block and else block for the if, and an end block+phi for the if result.
			auto thenBlock = llvm::BasicBlock::Create(context,"ifThen",llvmFunction);
			auto elseBlock = llvm::BasicBlock::Create(context,"ifElse",llvmFunction);
			auto endBlock = llvm::BasicBlock::Create(context,"ifElseEnd",llvmFunction);
			auto endPHI = createPHI(endBlock,imm.resultType);

			// Pop the if condition from the operand stack.
			auto condition = pop();
			irBuilder.CreateCondBr(coerceI32ToBool(condition),thenBlock,elseBlock);
			
			// Switch the IR builder to emit the then block.
			irBuilder.SetInsertPoint(thenBlock);

			// Push an ifThen control context that ultimately ends at the end block/phi, but may
			// be terminated by an else operator that changes the control context to the else block.
			pushControlStack(ControlContext::Type::ifThen,imm.resultType,endBlock,endPHI,elseBlock);
			
			// Push a branch target for the if end.
			pushBranchTarget(imm.resultType,endBlock,endPHI);
			
		}
		void beginElse(NoImm imm)
		{
			assert(controlStack.size());
			ControlContext& currentContext = controlStack.back();

			if(currentContext.isReachable)
			{
				// If the control context expects a result, take it from the operand stack and add it to the
				// control context's end PHI.
				if(currentContext.resultType != ResultType::none)
				{
					llvm::Value* result = pop();
					currentContext.endPHI->addIncoming(result,irBuilder.GetInsertBlock());
				}

				// Branch to the control context's end.
				irBuilder.CreateBr(currentContext.endBlock);
			}
			assert(stack.size() == currentContext.outerStackSize);

			// Switch the IR emitter to the else block.
			assert(currentContext.elseBlock);
			assert(currentContext.type == ControlContext::Type::ifThen);
			currentContext.elseBlock->moveAfter(irBuilder.GetInsertBlock());
			irBuilder.SetInsertPoint(currentContext.elseBlock);

			// Change the top of the control stack to an else clause.
			currentContext.type = ControlContext::Type::ifElse;
			currentContext.isReachable = currentContext.isElseReachable;
			currentContext.elseBlock = nullptr;
		}
		void end(NoImm)
		{
			assert(controlStack.size());
			ControlContext& currentContext = controlStack.back();

			if(currentContext.isReachable)
			{
				// If the control context yields a result, take the top of the operand stack and
				// add it to the control context's end PHI.
				if(currentContext.resultType != ResultType::none)
				{
					llvm::Value* result = pop();
					currentContext.endPHI->addIncoming(result,irBuilder.GetInsertBlock());
				}

				// Branch to the control context's end.
				irBuilder.CreateBr(currentContext.endBlock);
			}
			assert(stack.size() == currentContext.outerStackSize);

			if(currentContext.elseBlock)
			{
				// If this is the end of an if without an else clause, create a dummy else clause.
				currentContext.elseBlock->moveAfter(irBuilder.GetInsertBlock());
				irBuilder.SetInsertPoint(currentContext.elseBlock);
				irBuilder.CreateBr(currentContext.endBlock);
			}

			// Switch the IR emitter to the end block.
			currentContext.endBlock->moveAfter(irBuilder.GetInsertBlock());
			irBuilder.SetInsertPoint(currentContext.endBlock);

			if(currentContext.endPHI)
			{
				// If the control context yields a result, take the PHI that merges all the control flow
				// to the end and push it onto the operand stack.
				if(currentContext.endPHI->getNumIncomingValues()) { push(currentContext.endPHI); }
				else
				{
					// If there weren't any incoming values for the end PHI, remove it and push a dummy value.
					currentContext.endPHI->eraseFromParent();
					assert(currentContext.resultType != ResultType::none);
					push(typedZeroConstants[(uintp)asValueType(currentContext.resultType)]);
				}
			}

			// Pop and branch targets introduced by this control context.
			assert(currentContext.outerBranchTargetStackSize <= branchTargetStack.size());
			branchTargetStack.resize(currentContext.outerBranchTargetStackSize);

			// Pop this control context.
			controlStack.pop_back();
		}
		
		//
		// Control flow operators
		//
		
		BranchTarget& getBranchTargetByDepth(uintp depth)
		{
			assert(depth < branchTargetStack.size());
			return branchTargetStack[branchTargetStack.size() - depth - 1];
		}
		
		// This is called after unconditional control flow to indicate that operators following it are unreachable until the control stack is popped.
		void enterUnreachable()
		{
			// Unwind the operand stack to the outer control context.
			assert(controlStack.back().outerStackSize <= stack.size());
			stack.resize(controlStack.back().outerStackSize);

			// Mark the current control context as unreachable: this will cause the outer loop to stop dispatching operators to us
			// until an else/end for the current control context is reached.
			controlStack.back().isReachable = false;
		}
		
		void br_if(BranchImm imm)
		{
			// Pop the condition from operand stack.
			auto condition = pop();

			BranchTarget& target = getBranchTargetByDepth(imm.targetDepth);
			if(target.argumentType != ResultType::none)
			{
				// Use the stack top as the branch argument (don't pop it) and add it to the target phi's incoming values.
				llvm::Value* argument = getTopValue();
				target.phi->addIncoming(argument,irBuilder.GetInsertBlock());
			}

			// Create a new basic block for the case where the branch is not taken.
			auto falseBlock = llvm::BasicBlock::Create(context,"br_ifElse",llvmFunction);

			// Emit a conditional branch to either the falseBlock or the target block.
			irBuilder.CreateCondBr(coerceI32ToBool(condition),target.block,falseBlock);

			// Resume emitting instructions in the falseBlock.
			irBuilder.SetInsertPoint(falseBlock);
		}
		
		void br(BranchImm imm)
		{
			BranchTarget& target = getBranchTargetByDepth(imm.targetDepth);
			if(target.argumentType != ResultType::none)
			{
				// Pop the branch argument from the stack and add it to the target phi's incoming values.
				llvm::Value* argument = pop();
				target.phi->addIncoming(argument,irBuilder.GetInsertBlock());
			}

			// Branch to the target block.
			irBuilder.CreateBr(target.block);

			enterUnreachable();
		}
		void br_table(BranchTableImm imm)
		{
			// Pop the table index from the operand stack.
			auto index = pop();
			
			// Look up the default branch target, and assume its argument type applies to all targets.
			// (this is guaranteed by the validator)
			BranchTarget& defaultTarget = getBranchTargetByDepth(imm.defaultTargetDepth);
			const ResultType argumentType = defaultTarget.argumentType;
			llvm::Value* argument = nullptr;
			if(argumentType != ResultType::none)
			{
				// Pop the branch argument from the stack and add it to the default target phi's incoming values.
				argument = pop();
				defaultTarget.phi->addIncoming(argument,irBuilder.GetInsertBlock());
			}

			// Create a LLVM switch instruction.
			auto llvmSwitch = irBuilder.CreateSwitch(index,defaultTarget.block,(unsigned int)imm.targetDepths.size());

			for(uintp targetIndex = 0;targetIndex < imm.targetDepths.size();++targetIndex)
			{
				BranchTarget& target = getBranchTargetByDepth(imm.targetDepths[targetIndex]);

				// Add this target to the switch instruction.
				llvmSwitch->addCase(emitLiteral((uint32)targetIndex),target.block);

				if(argumentType != ResultType::none)
				{
					// If this is the first case in the table for this branch target, add the branch argument to
					// the target phi's incoming values.
					target.phi->addIncoming(argument,irBuilder.GetInsertBlock());
				}
			}

			enterUnreachable();
		}
		void ret(NoImm)
		{
			if(functionType->ret != ResultType::none)
			{
				// Pop the return value from the stack and add it to the return phi's incoming values.
				llvm::Value* result = pop();
				controlStack[0].endPHI->addIncoming(result,irBuilder.GetInsertBlock());
			}

			// Branch to the return block.
			irBuilder.CreateBr(controlStack[0].endBlock);

			enterUnreachable();
		}

		void unreachable(NoImm)
		{
			// Call an intrinsic that causes a trap, and insert the LLVM unreachable terminator.
			emitRuntimeIntrinsic("wavmIntrinsics.unreachableTrap",FunctionType::get(),{});
			irBuilder.CreateUnreachable();

			enterUnreachable();
		}

		//
		// Polymorphic operators
		//

		void drop(NoImm) { stack.pop_back(); }

		void select(NoImm)
		{
			auto condition = pop();
			auto falseValue = pop();
			auto trueValue = pop();
			push(irBuilder.CreateSelect(coerceI32ToBool(condition),trueValue,falseValue));
		}

		//
		// Call operators
		//

		void call(CallImm imm)
		{
			// Map the callee function index to either an imported function pointer or a function in this module.
			llvm::Value* callee;
			const FunctionType* calleeType;
			if(imm.functionIndex < moduleContext.importedFunctionPointers.size())
			{
				assert(imm.functionIndex < moduleContext.moduleInstance->functions.size());
				callee = moduleContext.importedFunctionPointers[imm.functionIndex];
				calleeType = moduleContext.moduleInstance->functions[imm.functionIndex]->type;
			}
			else
			{
				const uintp calleeIndex = imm.functionIndex - moduleContext.importedFunctionPointers.size();
				assert(calleeIndex < moduleContext.functionDefs.size());
				callee = moduleContext.functionDefs[calleeIndex];
				calleeType = module.types[module.functionDefs[calleeIndex].typeIndex];
			}

			// Pop the call arguments from the operand stack.
			auto llvmArgs = (llvm::Value**)alloca(sizeof(llvm::Value*) * calleeType->parameters.size());
			popMultiple(llvmArgs,calleeType->parameters.size());

			// Call the function.
			auto result = irBuilder.CreateCall(callee,llvm::ArrayRef<llvm::Value*>(llvmArgs,calleeType->parameters.size()));

			// Push the result on the operand stack.
			if(calleeType->ret != ResultType::none) { push(result); }
		}
		void call_indirect(CallIndirectImm imm)
		{
			assert(imm.typeIndex < module.types.size());
			
			auto calleeType = module.types[imm.typeIndex];
			auto functionPointerType = asLLVMType(calleeType)->getPointerTo()->getPointerTo();

			// Compile the function index.
			auto tableElementIndex = pop();
			
			// Compile the call arguments.
			auto llvmArgs = (llvm::Value**)alloca(sizeof(llvm::Value*) * calleeType->parameters.size());
			popMultiple(llvmArgs,calleeType->parameters.size());

			// Zero extend the function index to the pointer size.
			auto functionIndexZExt = irBuilder.CreateZExt(tableElementIndex,sizeof(uintp) == 4 ? llvmI32Type : llvmI64Type);
			
			// If the function index is larger than the function table size, trap.
			emitConditionalTrapIntrinsic(
				irBuilder.CreateICmpUGE(functionIndexZExt,moduleContext.defaultTableEndOffset),
				"wavmIntrinsics.indirectCallIndexOutOfBounds",FunctionType::get(),{});

			// Load the type for this table entry.
			auto functionTypePointerPointer = irBuilder.CreateInBoundsGEP(moduleContext.defaultTablePointer,{functionIndexZExt,emitLiteral((uint32)0)});
			auto functionTypePointer = irBuilder.CreateLoad(functionTypePointerPointer);
			auto llvmCalleeType = emitLiteralPointer(calleeType,llvmI8PtrType);
			
			// If the function type doesn't match, trap.
			emitConditionalTrapIntrinsic(
				irBuilder.CreateICmpNE(llvmCalleeType,functionTypePointer),
				"wavmIntrinsics.indirectCallSignatureMismatch",
				FunctionType::get(ResultType::none,{ValueType::i32,ValueType::i64,ValueType::i64}),
				{	tableElementIndex,
					irBuilder.CreatePtrToInt(llvmCalleeType,llvmI64Type),
					emitLiteral(reinterpret_cast<uint64>(moduleContext.moduleInstance->defaultTable))	}
				);

			// Call the function loaded from the table.
			auto functionPointerPointer = irBuilder.CreateInBoundsGEP(moduleContext.defaultTablePointer,{functionIndexZExt,emitLiteral((uint32)1)});
			auto functionPointer = irBuilder.CreateLoad(irBuilder.CreatePointerCast(functionPointerPointer,functionPointerType));
			auto result = irBuilder.CreateCall(functionPointer,llvm::ArrayRef<llvm::Value*>(llvmArgs,calleeType->parameters.size()));

			// Push the result on the operand stack.
			if(calleeType->ret != ResultType::none) { push(result); }
		}
		
		//
		// Local/global operators
		//

		void get_local(GetOrSetVariableImm imm)
		{
			assert(imm.variableIndex < localPointers.size());
			push(irBuilder.CreateLoad(localPointers[imm.variableIndex]));
		}
		void set_local(GetOrSetVariableImm imm)
		{
			assert(imm.variableIndex < localPointers.size());
			auto value = pop();
			irBuilder.CreateStore(value,localPointers[imm.variableIndex]);
		}
		void tee_local(GetOrSetVariableImm imm)
		{
			assert(imm.variableIndex < localPointers.size());
			auto value = getTopValue();
			irBuilder.CreateStore(value,localPointers[imm.variableIndex]);
		}
		
		void get_global(GetOrSetVariableImm imm)
		{
			assert(imm.variableIndex < moduleContext.globalPointers.size());
			push(irBuilder.CreateLoad(moduleContext.globalPointers[imm.variableIndex]));
		}
		void set_global(GetOrSetVariableImm imm)
		{
			assert(imm.variableIndex < moduleContext.globalPointers.size());
			auto value = pop();
			irBuilder.CreateStore(value,moduleContext.globalPointers[imm.variableIndex]);
		}

		//
		// Memory size operators
		// These just call out to wavmIntrinsics.growMemory/currentMemory, passing a pointer to the default memory for the module.
		//

		void grow_memory(MemoryImm)
		{
			auto deltaNumPages = pop();
			auto defaultMemoryObjectAsI64 = emitLiteral(reinterpret_cast<uint64>(moduleContext.moduleInstance->defaultMemory));
			auto previousNumPages = emitRuntimeIntrinsic(
				"wavmIntrinsics.growMemory",
				FunctionType::get(ResultType::i32,{ValueType::i32,ValueType::i64}),
				{deltaNumPages,defaultMemoryObjectAsI64});
			push(previousNumPages);
		}
		void current_memory(MemoryImm)
		{
			auto defaultMemoryObjectAsI64 = emitLiteral(reinterpret_cast<uint64>(moduleContext.moduleInstance->defaultMemory));
			auto currentNumPages = emitRuntimeIntrinsic(
				"wavmIntrinsics.currentMemory",
				FunctionType::get(ResultType::i32,{ValueType::i64}),
				{defaultMemoryObjectAsI64});
			push(currentNumPages);
		}

		//
		// Constant operators
		//

		#define EMIT_CONST(typeId,nativeType) void typeId##_const(LiteralImm<nativeType> imm) { push(emitLiteral(imm.value)); }
		EMIT_CONST(i32,int32) EMIT_CONST(i64,int64)
		EMIT_CONST(f32,float32) EMIT_CONST(f64,float64)

		//
		// Load/store operators
		//

		#define EMIT_LOAD_OP(valueTypeId,name,llvmMemoryType,conversionOp) void valueTypeId##_##name(LoadOrStoreImm imm) \
			{ \
				auto byteIndex = pop(); \
				auto pointer = coerceByteIndexToPointer(byteIndex,imm.offset,llvmMemoryType); \
				auto load = irBuilder.CreateLoad(pointer); \
				load->setAlignment(1<<imm.alignmentLog2); \
				load->setVolatile(true); \
				push(conversionOp(load,asLLVMType(ValueType::valueTypeId))); \
			}
		#define EMIT_STORE_OP(valueTypeId,name,llvmMemoryType,conversionOp) void valueTypeId##_##name(LoadOrStoreImm imm) \
			{ \
				auto value = pop(); \
				auto byteIndex = pop(); \
				auto pointer = coerceByteIndexToPointer(byteIndex,imm.offset,llvmMemoryType); \
				auto memoryValue = conversionOp(value,llvmMemoryType); \
				auto store = irBuilder.CreateStore(memoryValue,pointer); \
				store->setVolatile(true); \
				store->setAlignment(1<<imm.alignmentLog2); \
			}
			
		llvm::Value* identityConversion(llvm::Value* value,llvm::Type* type) { return value; }

		EMIT_LOAD_OP(i32,load8_s,llvmI8Type,irBuilder.CreateSExt)  EMIT_LOAD_OP(i32,load8_u,llvmI8Type,irBuilder.CreateZExt)
		EMIT_LOAD_OP(i32,load16_s,llvmI16Type,irBuilder.CreateSExt) EMIT_LOAD_OP(i32,load16_u,llvmI16Type,irBuilder.CreateZExt)
		EMIT_LOAD_OP(i64,load8_s,llvmI8Type,irBuilder.CreateSExt)  EMIT_LOAD_OP(i64,load8_u,llvmI8Type,irBuilder.CreateZExt)
		EMIT_LOAD_OP(i64,load16_s,llvmI16Type,irBuilder.CreateSExt)  EMIT_LOAD_OP(i64,load16_u,llvmI16Type,irBuilder.CreateZExt)
		EMIT_LOAD_OP(i64,load32_s,llvmI32Type,irBuilder.CreateSExt)  EMIT_LOAD_OP(i64,load32_u,llvmI32Type,irBuilder.CreateZExt)

		EMIT_LOAD_OP(i32,load,llvmI32Type,identityConversion) EMIT_LOAD_OP(i64,load,llvmI64Type,identityConversion)
		EMIT_LOAD_OP(f32,load,llvmF32Type,identityConversion) EMIT_LOAD_OP(f64,load,llvmF64Type,identityConversion)

		EMIT_STORE_OP(i32,store8,llvmI8Type,irBuilder.CreateTrunc) EMIT_STORE_OP(i64,store8,llvmI8Type,irBuilder.CreateTrunc)
		EMIT_STORE_OP(i32,store16,llvmI16Type,irBuilder.CreateTrunc) EMIT_STORE_OP(i64,store16,llvmI16Type,irBuilder.CreateTrunc)
		EMIT_STORE_OP(i32,store,llvmI32Type,irBuilder.CreateTrunc) EMIT_STORE_OP(i64,store32,llvmI32Type,irBuilder.CreateTrunc)
		EMIT_STORE_OP(i64,store,llvmI64Type,identityConversion)
		EMIT_STORE_OP(f32,store,llvmF32Type,identityConversion) EMIT_STORE_OP(f64,store,llvmF64Type,identityConversion)

		//
		// Numeric operator macros
		//

		#define EMIT_BINARY_OP(typeId,name,emitCode) void typeId##_##name(NoImm) \
			{ \
				UNUSED const ValueType type = ValueType::typeId; \
				auto right = pop(); \
				auto left = pop(); \
				push(emitCode); \
			}
		#define EMIT_INT_BINARY_OP(name,emitCode) EMIT_BINARY_OP(i32,name,emitCode) EMIT_BINARY_OP(i64,name,emitCode)
		#define EMIT_FP_BINARY_OP(name,emitCode) EMIT_BINARY_OP(f32,name,emitCode) EMIT_BINARY_OP(f64,name,emitCode)

		#define EMIT_UNARY_OP(typeId,name,emitCode) void typeId##_##name(NoImm) \
			{ \
				UNUSED const ValueType type = ValueType::typeId; \
				auto operand = pop(); \
				push(emitCode); \
			}
		#define EMIT_INT_UNARY_OP(name,emitCode) EMIT_UNARY_OP(i32,name,emitCode) EMIT_UNARY_OP(i64,name,emitCode)
		#define EMIT_FP_UNARY_OP(name,emitCode) EMIT_UNARY_OP(f32,name,emitCode) EMIT_UNARY_OP(f64,name,emitCode)

		//
		// Int operators
		//

		llvm::Value* emitSRem(ValueType type,llvm::Value* left,llvm::Value* right)
		{
			// Trap if the dividend is zero.
			trapDivideByZero(type,right); 

			// LLVM's srem has undefined behavior where WebAssembly's rem_s defines that it should not trap if the corresponding
			// division would overflow a signed integer. To avoid this case, we just branch around the srem if the INT_MAX%-1 case
			// that overflows is detected.
			auto preOverflowBlock = irBuilder.GetInsertBlock();
			auto noOverflowBlock = llvm::BasicBlock::Create(context,"sremNoOverflow",llvmFunction);
			auto endBlock = llvm::BasicBlock::Create(context,"sremEnd",llvmFunction);
			auto noOverflow = irBuilder.CreateOr(
				irBuilder.CreateICmpNE(left,type == ValueType::i32 ? emitLiteral((uint32)INT32_MIN) : emitLiteral((uint64)INT64_MIN)),
				irBuilder.CreateICmpNE(right,type == ValueType::i32 ? emitLiteral((uint32)-1) : emitLiteral((uint64)-1))
				);
			irBuilder.CreateCondBr(noOverflow,noOverflowBlock,endBlock,moduleContext.likelyTrueBranchWeights);

			irBuilder.SetInsertPoint(noOverflowBlock);
			auto noOverflowValue = irBuilder.CreateSRem(left,right);
			irBuilder.CreateBr(endBlock);

			irBuilder.SetInsertPoint(endBlock);
			auto phi = irBuilder.CreatePHI(asLLVMType(type),2);
			phi->addIncoming(typedZeroConstants[(uintp)type],preOverflowBlock);
			phi->addIncoming(noOverflowValue,noOverflowBlock);
			return phi;
		}
		
		llvm::Value* emitShiftCountMask(ValueType type,llvm::Value* shiftCount)
		{
			// LLVM's shifts have undefined behavior where WebAssembly specifies that the shift count will wrap numbers
			// grather than the bit count of the operands. This matches x86's native shift instructions, but explicitly mask
			// the shift count anyway to support other platforms, and ensure the optimizer doesn't take advantage of the UB.
			auto bitsMinusOne = irBuilder.CreateZExt(emitLiteral((uint8)(getTypeBitWidth(type) - 1)),asLLVMType(type));
			return irBuilder.CreateAnd(shiftCount,bitsMinusOne);
		}

		llvm::Value* emitRotl(ValueType type,llvm::Value* left,llvm::Value* right)
		{
			auto bitWidthMinusRight = irBuilder.CreateSub(
				irBuilder.CreateZExt(emitLiteral(getTypeBitWidth(type)),asLLVMType(type)),
				right
				);
			return irBuilder.CreateOr(
				irBuilder.CreateShl(left,emitShiftCountMask(type,right)),
				irBuilder.CreateLShr(left,emitShiftCountMask(type,bitWidthMinusRight))
				);
		}
		
		llvm::Value* emitRotr(ValueType type,llvm::Value* left,llvm::Value* right)
		{
			auto bitWidthMinusRight = irBuilder.CreateSub(
				irBuilder.CreateZExt(emitLiteral(getTypeBitWidth(type)),asLLVMType(type)),
				right
				);
			return irBuilder.CreateOr(
				irBuilder.CreateShl(left,emitShiftCountMask(type,bitWidthMinusRight)),
				irBuilder.CreateLShr(left,emitShiftCountMask(type,right))
				);
		}

		EMIT_INT_BINARY_OP(add,irBuilder.CreateAdd(left,right))
		EMIT_INT_BINARY_OP(sub,irBuilder.CreateSub(left,right))
		EMIT_INT_BINARY_OP(mul,irBuilder.CreateMul(left,right))
		EMIT_INT_BINARY_OP(and,irBuilder.CreateAnd(left,right))
		EMIT_INT_BINARY_OP(or,irBuilder.CreateOr(left,right))
		EMIT_INT_BINARY_OP(xor,irBuilder.CreateXor(left,right))
		EMIT_INT_BINARY_OP(rotr,emitRotr(type,left,right))
		EMIT_INT_BINARY_OP(rotl,emitRotl(type,left,right))
			
		// Divides use trapDivideByZero to avoid the undefined behavior in LLVM's division instructions.
		EMIT_INT_BINARY_OP(div_s, (trapDivideByZero(type,right), irBuilder.CreateSDiv(left,right)) )
		EMIT_INT_BINARY_OP(div_u, (trapDivideByZero(type,right), irBuilder.CreateUDiv(left,right)) )
		EMIT_INT_BINARY_OP(rem_u, (trapDivideByZero(type,right), irBuilder.CreateURem(left,right)) )
		EMIT_INT_BINARY_OP(rem_s,emitSRem(type,left,right))

		// Explicitly mask the shift amount operand to the word size to avoid LLVM's undefined behavior.
		EMIT_INT_BINARY_OP(shl,irBuilder.CreateShl(left,emitShiftCountMask(type,right)))
		EMIT_INT_BINARY_OP(shr_s,irBuilder.CreateAShr(left,emitShiftCountMask(type,right)))
		EMIT_INT_BINARY_OP(shr_u,irBuilder.CreateLShr(left,emitShiftCountMask(type,right)))
		
		EMIT_INT_BINARY_OP(eq,coerceBoolToI32(irBuilder.CreateICmpEQ(left,right)))
		EMIT_INT_BINARY_OP(ne,coerceBoolToI32(irBuilder.CreateICmpNE(left,right)))
		EMIT_INT_BINARY_OP(lt_s,coerceBoolToI32(irBuilder.CreateICmpSLT(left,right)))
		EMIT_INT_BINARY_OP(lt_u,coerceBoolToI32(irBuilder.CreateICmpULT(left,right)))
		EMIT_INT_BINARY_OP(le_s,coerceBoolToI32(irBuilder.CreateICmpSLE(left,right)))
		EMIT_INT_BINARY_OP(le_u,coerceBoolToI32(irBuilder.CreateICmpULE(left,right)))
		EMIT_INT_BINARY_OP(gt_s,coerceBoolToI32(irBuilder.CreateICmpSGT(left,right)))
		EMIT_INT_BINARY_OP(gt_u,coerceBoolToI32(irBuilder.CreateICmpUGT(left,right)))
		EMIT_INT_BINARY_OP(ge_s,coerceBoolToI32(irBuilder.CreateICmpSGE(left,right)))
		EMIT_INT_BINARY_OP(ge_u,coerceBoolToI32(irBuilder.CreateICmpUGE(left,right)))

		EMIT_INT_UNARY_OP(clz,irBuilder.CreateCall(getLLVMIntrinsic({operand->getType()},llvm::Intrinsic::ctlz),llvm::ArrayRef<llvm::Value*>({operand,emitLiteral(false)})))
		EMIT_INT_UNARY_OP(ctz,irBuilder.CreateCall(getLLVMIntrinsic({operand->getType()},llvm::Intrinsic::cttz),llvm::ArrayRef<llvm::Value*>({operand,emitLiteral(false)})))
		EMIT_INT_UNARY_OP(popcnt,irBuilder.CreateCall(getLLVMIntrinsic({operand->getType()},llvm::Intrinsic::ctpop),llvm::ArrayRef<llvm::Value*>({operand})))
		EMIT_INT_UNARY_OP(eqz,coerceBoolToI32(irBuilder.CreateICmpEQ(operand,typedZeroConstants[(uintp)type])))

		//
		// FP operators
		//

		EMIT_FP_BINARY_OP(add,irBuilder.CreateFAdd(left,right))
		EMIT_FP_BINARY_OP(sub,irBuilder.CreateFSub(left,right))
		EMIT_FP_BINARY_OP(mul,irBuilder.CreateFMul(left,right))
		EMIT_FP_BINARY_OP(div,irBuilder.CreateFDiv(left,right))
		EMIT_FP_BINARY_OP(copysign,irBuilder.CreateCall(getLLVMIntrinsic({left->getType()},llvm::Intrinsic::copysign),llvm::ArrayRef<llvm::Value*>({left,right})))

		EMIT_FP_UNARY_OP(neg,irBuilder.CreateFNeg(operand))
		EMIT_FP_UNARY_OP(abs,irBuilder.CreateCall(getLLVMIntrinsic({operand->getType()},llvm::Intrinsic::fabs),llvm::ArrayRef<llvm::Value*>({operand})))
		EMIT_FP_UNARY_OP(sqrt,irBuilder.CreateCall(getLLVMIntrinsic({operand->getType()},llvm::Intrinsic::sqrt),llvm::ArrayRef<llvm::Value*>({operand})))

		EMIT_FP_BINARY_OP(eq,coerceBoolToI32(irBuilder.CreateFCmpOEQ(left,right)))
		EMIT_FP_BINARY_OP(ne,coerceBoolToI32(irBuilder.CreateFCmpUNE(left,right)))
		EMIT_FP_BINARY_OP(lt,coerceBoolToI32(irBuilder.CreateFCmpOLT(left,right)))
		EMIT_FP_BINARY_OP(le,coerceBoolToI32(irBuilder.CreateFCmpOLE(left,right)))
		EMIT_FP_BINARY_OP(gt,coerceBoolToI32(irBuilder.CreateFCmpOGT(left,right)))
		EMIT_FP_BINARY_OP(ge,coerceBoolToI32(irBuilder.CreateFCmpOGE(left,right)))

		EMIT_UNARY_OP(i32,wrap_i64,irBuilder.CreateTrunc(operand,llvmI32Type))
		EMIT_UNARY_OP(i64,extend_s_i32,irBuilder.CreateSExt(operand,llvmI64Type))
		EMIT_UNARY_OP(i64,extend_u_i32,irBuilder.CreateZExt(operand,llvmI64Type))

		EMIT_FP_UNARY_OP(convert_s_i32,irBuilder.CreateSIToFP(operand,asLLVMType(type)))
		EMIT_FP_UNARY_OP(convert_s_i64,irBuilder.CreateSIToFP(operand,asLLVMType(type)))
		EMIT_FP_UNARY_OP(convert_u_i32,irBuilder.CreateUIToFP(operand,asLLVMType(type)))
		EMIT_FP_UNARY_OP(convert_u_i64,irBuilder.CreateUIToFP(operand,asLLVMType(type)))

		EMIT_UNARY_OP(f32,demote_f64,irBuilder.CreateFPTrunc(operand,llvmF32Type))
		EMIT_UNARY_OP(f64,promote_f32,irBuilder.CreateFPExt(operand,llvmF64Type))
		EMIT_UNARY_OP(f32,reinterpret_i32,irBuilder.CreateBitCast(operand,llvmF32Type))
		EMIT_UNARY_OP(f64,reinterpret_i64,irBuilder.CreateBitCast(operand,llvmF64Type))
		EMIT_UNARY_OP(i32,reinterpret_f32,irBuilder.CreateBitCast(operand,llvmI32Type))
		EMIT_UNARY_OP(i64,reinterpret_f64,irBuilder.CreateBitCast(operand,llvmI64Type))

		// These operations don't match LLVM's semantics exactly, so just call out to C++ implementations.
		EMIT_FP_BINARY_OP(min,emitRuntimeIntrinsic("wavmIntrinsics.floatMin",FunctionType::get(asResultType(type),{type,type}),{left,right}))
		EMIT_FP_BINARY_OP(max,emitRuntimeIntrinsic("wavmIntrinsics.floatMax",FunctionType::get(asResultType(type),{type,type}),{left,right}))
		EMIT_FP_UNARY_OP(ceil,emitRuntimeIntrinsic("wavmIntrinsics.floatCeil",FunctionType::get(asResultType(type),{type}),{operand}))
		EMIT_FP_UNARY_OP(floor,emitRuntimeIntrinsic("wavmIntrinsics.floatFloor",FunctionType::get(asResultType(type),{type}),{operand}))
		EMIT_FP_UNARY_OP(trunc,emitRuntimeIntrinsic("wavmIntrinsics.floatTrunc",FunctionType::get(asResultType(type),{type}),{operand}))
		EMIT_FP_UNARY_OP(nearest,emitRuntimeIntrinsic("wavmIntrinsics.floatNearest",FunctionType::get(asResultType(type),{type}),{operand}))
		EMIT_INT_UNARY_OP(trunc_s_f32,emitRuntimeIntrinsic("wavmIntrinsics.floatToSignedInt",FunctionType::get(asResultType(type),{ValueType::f32}),{operand}))
		EMIT_INT_UNARY_OP(trunc_s_f64,emitRuntimeIntrinsic("wavmIntrinsics.floatToSignedInt",FunctionType::get(asResultType(type),{ValueType::f64}),{operand}))
		EMIT_INT_UNARY_OP(trunc_u_f32,emitRuntimeIntrinsic("wavmIntrinsics.floatToUnsignedInt",FunctionType::get(asResultType(type),{ValueType::f32}),{operand}))
		EMIT_INT_UNARY_OP(trunc_u_f64,emitRuntimeIntrinsic("wavmIntrinsics.floatToUnsignedInt",FunctionType::get(asResultType(type),{ValueType::f64}),{operand}))
	};
	
	// A do-nothing visitor used to decode past unreachable operators (but supporting logging, and passing the end operator through).
	struct UnreachableOpVisitor
	{
		UnreachableOpVisitor(EmitFunctionContext& inContext): context(inContext), unreachableControlDepth(0) {}
		#define VISIT_OP(encoding,name,Imm) void name(Imm imm) {}
		ENUM_NONCONTROL_OPS(VISIT_OP)
		VISIT_OP(_,unknown,Opcode)
		#undef VISIT_OP

		void nop(NoImm) {}
		void select(NoImm) {}
		void br(BranchImm) {}
		void br_if(BranchImm) {}
		void br_table(BranchTableImm) {}
		void ret(NoImm) {}
		void unreachable(NoImm) {}
		void drop(NoImm) {}
		void call(CallImm) {}
		void call_indirect(CallIndirectImm) {}

		// Keep track of control structure nesting level in unreachable code, so we know when we reach the end of the unreachable code.
		void beginBlock(ControlStructureImm) { ++unreachableControlDepth; }
		void beginLoop(ControlStructureImm) { ++unreachableControlDepth; }
		void beginIf(ControlStructureImm) { ++unreachableControlDepth; }

		// If an else or end opcode would signal an end to the unreachable code, then pass it through to the IR emitter.
		void beginElse(NoImm imm)
		{
			if(!unreachableControlDepth) { context.beginElse(imm); }
		}
		void end(NoImm imm)
		{
			if(!unreachableControlDepth) { context.end(imm); }
			else { --unreachableControlDepth; }
		}

		void logOperator(const std::string& operatorDescription) { context.logOperator(operatorDescription); }
	private:
		EmitFunctionContext& context;
		uintp unreachableControlDepth;
	};

	void EmitFunctionContext::emit()
	{
		// Create debug info for the function.
		llvm::SmallVector<llvm::Metadata*,10> diFunctionParameterTypes;
		for(auto parameterType : functionType->parameters) { diFunctionParameterTypes.push_back(moduleContext.diValueTypes[(uintp)parameterType]); }
		auto diFunctionType = moduleContext.diBuilder.createSubroutineType(moduleContext.diBuilder.getOrCreateTypeArray(diFunctionParameterTypes));
		diFunction = moduleContext.diBuilder.createFunction(
			moduleContext.diModuleScope,
			functionInstance->debugName,
			llvmFunction->getName(),
			moduleContext.diModuleScope,
			0,
			diFunctionType,
			false,
			true,
			0);
		llvmFunction->setSubprogram(diFunction);

		// Create the return basic block, and push the root control context for the function.
		auto returnBlock = llvm::BasicBlock::Create(context,"return",llvmFunction);
		auto returnPHI = createPHI(returnBlock,functionType->ret);
		pushControlStack(ControlContext::Type::function,functionType->ret,returnBlock,returnPHI);
		pushBranchTarget(functionType->ret,returnBlock,returnPHI);

		// Create an initial basic block for the function.
		auto entryBasicBlock = llvm::BasicBlock::Create(context,"entry",llvmFunction);
		irBuilder.SetInsertPoint(entryBasicBlock);

		// If enabled, emit a call to the WAVM function enter hook (for debugging).
		if(ENABLE_FUNCTION_ENTER_EXIT_HOOKS)
		{
			emitRuntimeIntrinsic(
				"wavmIntrinsics.debugEnterFunction",
				FunctionType::get(ResultType::none,{ValueType::i64}),
				{emitLiteral(reinterpret_cast<uint64>(functionInstance))}
				);
		}

		// Create and initialize allocas for all the locals and parameters.
		auto llvmArgIt = llvmFunction->arg_begin();
		for(uintp localIndex = 0;localIndex < functionType->parameters.size() + function.nonParameterLocalTypes.size();++localIndex)
		{
			auto localType = localIndex < functionType->parameters.size()
				? functionType->parameters[localIndex]
				: function.nonParameterLocalTypes[localIndex - functionType->parameters.size()];
			auto localPointer = irBuilder.CreateAlloca(asLLVMType(localType),nullptr,"");
			localPointers.push_back(localPointer);

			if(localIndex < functionType->parameters.size())
			{
				// Copy the parameter value into the local that stores it.
				irBuilder.CreateStore((llvm::Argument*)llvmArgIt,localPointer);
				++llvmArgIt;
			}
			else
			{
				// Initialize non-parameter locals to zero.
				irBuilder.CreateStore(typedZeroConstants[(uintp)localType],localPointer);
			}
		}

		// Decode the WebAssembly opcodes and emit LLVM IR for them.
		Serialization::MemoryInputStream codeStream(module.code.data() + function.code.offset,function.code.numBytes);
		OperationDecoder decoder(codeStream);
		UnreachableOpVisitor unreachableOpVisitor(*this);
		OperatorLoggingProxy<EmitFunctionContext> loggingProxy(module,*this);
		OperatorLoggingProxy<UnreachableOpVisitor> unreachableLoggingProxy(module,unreachableOpVisitor);
		uintp opIndex = 0;
		while(decoder && controlStack.size())
		{
			irBuilder.SetCurrentDebugLocation(llvm::DILocation::get(context,(unsigned int)opIndex++,0,diFunction));
			if(ENABLE_LOGGING)
			{
				if(controlStack.back().isReachable) { decoder.decodeOp(loggingProxy); }
				else { decoder.decodeOp(unreachableLoggingProxy); }
			}
			else
			{
				if(controlStack.back().isReachable) { decoder.decodeOp(*this); }
				else { decoder.decodeOp(unreachableOpVisitor); }
			}
		};
		assert(irBuilder.GetInsertBlock() == returnBlock);
		
		// If enabled, emit a call to the WAVM function enter hook (for debugging).
		if(ENABLE_FUNCTION_ENTER_EXIT_HOOKS)
		{
			emitRuntimeIntrinsic(
				"wavmIntrinsics.debugExitFunction",
				FunctionType::get(ResultType::none,{ValueType::i64}),
				{emitLiteral(reinterpret_cast<uint64>(functionInstance))}
				);
		}

		// Emit the function return.
		if(functionType->ret == ResultType::none) { irBuilder.CreateRetVoid(); }
		else { irBuilder.CreateRet(pop()); }
	}

	llvm::Module* EmitModuleContext::emit()
	{
		Core::Timer emitTimer;

		// Create literals for the default memory base and mask.
		if(moduleInstance->defaultMemory)
		{
			defaultMemoryBase = emitLiteralPointer(moduleInstance->defaultMemory->baseAddress,llvmI8PtrType);
			const uintp defaultMemoryAddressMaskValue = uintp(moduleInstance->defaultMemory->endOffset) - 1;
			defaultMemoryAddressMask = emitLiteral(defaultMemoryAddressMaskValue);
		}
		else { defaultMemoryBase = defaultMemoryAddressMask = nullptr; }

		// Set up the LLVM values used to access the global table.
		if(moduleInstance->defaultTable)
		{
			auto tableElementType = llvm::StructType::get(context,{
				llvmI8PtrType,
				llvmI8PtrType
				});
			defaultTablePointer = emitLiteralPointer(moduleInstance->defaultTable->baseAddress,tableElementType->getPointerTo());
			defaultTableEndOffset = emitLiteral((uintp)moduleInstance->defaultTable->endOffset);
		}
		else
		{
			defaultTablePointer = defaultTableEndOffset = nullptr;
		}

		// Create LLVM pointer constants for the module's imported functions.
		for(uintp functionIndex = 0;functionIndex < moduleInstance->functions.size() - module.functionDefs.size();++functionIndex)
		{
			const FunctionInstance* functionInstance = moduleInstance->functions[functionIndex];
			importedFunctionPointers.push_back(emitLiteralPointer(functionInstance->nativeFunction,asLLVMType(functionInstance->type)->getPointerTo()));
		}

		// Create LLVM pointer constants for the module's globals.
		for(auto global : moduleInstance->globals)
		{ globalPointers.push_back(emitLiteralPointer(&global->value,asLLVMType(global->type.valueType)->getPointerTo())); }
		
		// Create the LLVM functions.
		functionDefs.resize(module.functionDefs.size());
		for(uintp functionDefIndex = 0;functionDefIndex < module.functionDefs.size();++functionDefIndex)
		{
			const Function& function = module.functionDefs[functionDefIndex];
			const FunctionType* functionType = module.types[function.typeIndex];
			auto llvmFunctionType = asLLVMType(functionType);
			auto externalName = getExternalFunctionName(moduleInstance,functionDefIndex);
			functionDefs[functionDefIndex] = llvm::Function::Create(llvmFunctionType,llvm::Function::ExternalLinkage,externalName,llvmModule);
		}

		// Compile each function in the module.
		for(uintp functionDefIndex = 0;functionDefIndex < module.functionDefs.size();++functionDefIndex)
		{ EmitFunctionContext(*this,module,module.functionDefs[functionDefIndex],moduleInstance->functionDefs[functionDefIndex],functionDefs[functionDefIndex]).emit(); }
		
		// Finalize the debug info.
		diBuilder.finalize();

		Log::logRatePerSecond("Emitted LLVM IR",emitTimer,(float64)llvmModule->size(),"functions");

		return llvmModule;
	}

	llvm::Module* emitModule(const Module& module,ModuleInstance* moduleInstance)
	{
		return EmitModuleContext(module,moduleInstance).emit();
	}
}
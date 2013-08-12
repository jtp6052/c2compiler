/* Copyright 2013 Bas van den Berg
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <llvm/Module.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/LLVMContext.h>
#include <llvm/Function.h>
#include <llvm/BasicBlock.h>
#include <vector>
// for SmallString
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
// for tool_output_file
#include <llvm/Support/ToolOutputFile.h>
// for WriteBitcodeToFile
#include <llvm/Bitcode/ReaderWriter.h>
// For verifyModule()
#include <llvm/Analysis/Verifier.h>
// TODO REMOVE
#include <stdio.h>


#include "CodeGenModule.h"
#include "CodeGenFunction.h"
#include "Package.h"
#include "AST.h"
#include "Type.h"
#include "Decl.h"
#include "Expr.h"
#include "StringBuilder.h"

//#define DEBUG_CODEGEN

using namespace C2;
using namespace llvm;

CodeGenModule::CodeGenModule(const Package* pkg_)
    : pkg(pkg_)
    , context(llvm::getGlobalContext())
    , module(new llvm::Module(pkg->getName(), context))
    , builder(context)
{}

CodeGenModule::~CodeGenModule() {
    delete module;
}

void CodeGenModule::addEntry(const std::string& filename, AST& ast) {
    entries.push_back(Entry(filename, ast));
}

void CodeGenModule::generate() {
    // TODO use ASTVisitor for this?

    // pass 1: generate all function proto's
    for (EntriesIter iter = entries.begin(); iter != entries.end(); ++iter) {
#ifdef DEBUG_CODEGEN
        printf("CodeGen for %s - pass 1\n", iter->filename->c_str());
#endif
        AST* ast = iter->ast;
        for (unsigned int i=0; i<ast->getNumDecls(); i++) {
            Decl* decl = ast->getDecl(i);
            if (isa<FunctionDecl>(decl)) EmitFunctionProto(decl);
        }
    }

    // pass 2: generate all function bodies (and possibly external function decls)
    for (EntriesIter iter = entries.begin(); iter != entries.end(); ++iter) {
#ifdef DEBUG_CODEGEN
        printf("CodeGen for %s - pass 2\n", iter->filename->c_str());
#endif
        AST* ast = iter->ast;
        for (unsigned int i=0; i<ast->getNumDecls(); i++) {
            Decl* decl = ast->getDecl(i);
            EmitTopLevelDecl(decl);
        }
    }
}

bool CodeGenModule::verify() {
    if (verifyModule(*module)) {
        errs() << "Error in Module!\n";
        return false;
    }
    return true;
}

void CodeGenModule::dump() {
    fprintf(stderr, "-----------------------------------------------------------------------\n");
    module->dump();
}

void CodeGenModule::write(const std::string& target, const std::string& name) {
    // write IR Module to output/<target>/<package>.ll
    StringBuilder filename;
    filename << "output/" << target << '/';
    bool existed;
    llvm::Twine path(filename);
    if (llvm::sys::fs::create_directories(path, existed) != llvm::errc::success) {
        fprintf(stderr, "Could not create directory: %s\n", (const char*)filename);
        return;
    }

    filename << name << ".ll";
    std::string ErrorInfo;
    llvm::raw_fd_ostream OS((const char*)filename, ErrorInfo);
    if (!ErrorInfo.empty()) {
        fprintf(stderr, "%s\n", ErrorInfo.c_str());
        return;
    }
    module->print(OS, 0);
    printf("written %s\n", (const char*)filename);
#if 0
    // print to binary file
    {
        std::string ErrInfo;
        StringBuilder filename;
        filename << "output/" << target << '/' << name;
        const char* outfile = (const char*)filename;
        tool_output_file Out(outfile, ErrInfo, raw_fd_ostream::F_Binary);
        if (!ErrInfo.empty()) {
            fprintf(stderr, "Could not open file %s\n", outfile);
            return;
        }
        llvm::WriteBitcodeToFile(module, Out.os());
        Out.os().close();
        if (Out.os().has_error()) {
            fprintf(stderr, "Error writing file %s\n", outfile);
        }
        Out.keep();
        printf("written %s\n", outfile);
    }
#endif
}

void CodeGenModule::EmitFunctionProto(Decl* D) {
    FunctionDecl* F = cast<FunctionDecl>(D);
    assert(F);
    CodeGenFunction cgf(*this, F);
    llvm::Function* proto = cgf.generateProto(pkg->getName());
    F->setIRProto(proto);
}

void CodeGenModule::EmitTopLevelDecl(Decl* D) {
    switch (D->getKind()) {
    case DECL_FUNC:
        {
            FunctionDecl* F = cast<FunctionDecl>(D);
            CodeGenFunction cgf(*this, F);
            cgf.generateBody(F->getIRProto());
        }
        break;
    case DECL_VAR:
        {
            VarDecl* Var = cast<VarDecl>(D);
            QualType qt = Var->getType();
            llvm::Type* type = ConvertType(qt.getTypePtr());
            bool constant = false;
            llvm::GlobalValue::LinkageTypes ltype = llvm::GlobalValue::InternalLinkage;
            if (Var->isPublic()) ltype = llvm::GlobalValue::ExternalLinkage;
            // TODO use correct arguments for constant and Initializer
            // NOTE: getName() doesn't have to be virtual here

            const Expr* I = Var->getInitValue();
            llvm::Constant* init;
            if (I) {
                init = EvaluateExprAsConstant(I);
            } else {
                // ALWAYS initialize globals
                // TODO dynamic width
                init = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0, true);
            }
            //new llvm::GlobalVariable(type, constant, ltype, 0, Var->getName());
            new llvm::GlobalVariable(*module, type, constant, ltype, init, Var->getName());
            //llvm::GlobalVariable* global =
            //    new llvm::GlobalVariable(module, type, constant, ltype, 0, Var->getName());
        }
        break;
    case DECL_ENUMVALUE:
        assert(0 && "TODO");
        break;
    case DECL_TYPE:
        {
            TypeDecl* TD = cast<TypeDecl>(D);
            QualType QT = TD->getType();
            // NOTE: only generate code for struct/union types (even this is optional)
            if (QT.isStructOrUnionType()) {
#if 0
                // TEMP try some ints
                std::vector<llvm::Type *> putsArgs;
                putsArgs.push_back(builder.getInt32Ty());
                llvm::ArrayRef<llvm::Type*>  argsRef(putsArgs);
                llvm::StructType* s = llvm::StructType::create(getContext(), putsArgs, TD->getName());
                //const Type* T = QT.getTypePtr();
#endif
            }
        }
        break;
    case DECL_ARRAYVALUE:
        assert(0 && "TODO arrayvalue");
        break;
    case DECL_USE:
        // nothing needed
        break;
    }
}

llvm::Type* CodeGenModule::ConvertType(const C2::Type* type) {
    switch (type->getKind()) {
    case Type::BUILTIN:
        {
            // TODO make u8/16/32 unsigned
            switch (type->getBuiltinType()) {
            case TYPE_U8:       return builder.getInt8Ty();
            case TYPE_U16:      return builder.getInt16Ty();
            case TYPE_U32:      return builder.getInt32Ty();
            case TYPE_U64:      return builder.getInt64Ty();
            case TYPE_I8:       return builder.getInt8Ty();
            case TYPE_I16:      return builder.getInt16Ty();
            case TYPE_I32:      return builder.getInt32Ty();
            case TYPE_I64:      return builder.getInt64Ty();
            case TYPE_INT:      return builder.getInt32Ty();
            case TYPE_STRING:
                return 0;  // TODO remove this type?
            case TYPE_F32:      return builder.getFloatTy();
            case TYPE_F64:      return builder.getFloatTy(); // TODO find correct type (double?)
            case TYPE_BOOL:     return builder.getInt1Ty();
            case TYPE_VOID:     return builder.getVoidTy();
            }
        }
        break;
    case Type::USER:
        return ConvertType(type->getRefType().getTypePtr());
    case Type::STRUCT:
    case Type::UNION:
        assert(0 && "TODO union/struct type");
    case Type::ENUM:
        // TODO use canonical type, for now use Int32()
        return builder.getInt32Ty();
    case Type::FUNC:
        assert(0 && "TODO func type");
        break;
    case Type::POINTER:
        {
            llvm::Type* tt = ConvertType(type->getRefType().getTypePtr());
            return tt->getPointerTo();
        }
    case Type::ARRAY:
        {
            // Hmm for function args, array are simply converted to pointers, do that for now
            // array: use type = ArrayType::get(elementType, numElements)
            llvm::Type* tt = ConvertType(type->getRefType().getTypePtr());
            return tt->getPointerTo();
        }
    }

    return 0;
}

llvm::Function* CodeGenModule::createExternal(const Package* P, const std::string& name) {
    Decl* D = P->findSymbol(name);
    assert(D);
    FunctionDecl* F = cast<FunctionDecl>(D);
    assert(F);
    CodeGenFunction cgf(*this, F);
    llvm::Function* proto = cgf.generateProto(P->getCName());
    return proto;
}

llvm::Constant* CodeGenModule::EvaluateExprAsConstant(const Expr *E) {
    // NOTE: for now only support numbers and convert those to bools
    IntegerLiteral* N = ExprCaster<IntegerLiteral>::getType(E);
    assert(N && "Only support constants for now");
    // Get Width/signed from CanonicalType?
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), N->Value.getSExtValue(), true);
}

/// EvaluateExprAsBool - Perform the usual unary conversions on the specified
/// expression and compare the result against zero, returning an Int1Ty value.
llvm::Value *CodeGenModule::EvaluateExprAsBool(const Expr *E) {
    // NOTE: for now only support numbers and convert those to bools
    IntegerLiteral* N = ExprCaster<IntegerLiteral>::getType(E);
    assert(N && "Only support constants for now");
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(context), N->Value.getSExtValue(), true);

#if 0
  if (const MemberPointerType *MPT = E->getType()->getAs<MemberPointerType>()) {
    llvm::Value *MemPtr = EmitScalarExpr(E);
    return CGM.getCXXABI().EmitMemberPointerIsNotNull(*this, MemPtr, MPT);
  }

  QualType BoolTy = getContext().BoolTy;
  if (!E->getType()->isAnyComplexType())
    return EmitScalarConversion(EmitScalarExpr(E), E->getType(), BoolTy);

  return EmitComplexToScalarConversion(EmitComplexExpr(E), E->getType(),BoolTy);
#endif
}


/* Copyright 2013,2014,2015 Bas van den Berg
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

#include <stdio.h>

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/APInt.h>
#include <clang/Parse/ParseDiagnostic.h>
#include <clang/Sema/SemaDiagnostic.h>

#include "Analyser/FunctionAnalyser.h"
#include "Analyser/TypeResolver.h"
#include "Analyser/LiteralAnalyser.h"
#include "Analyser/Scope.h"
#include "Analyser/AnalyserConstants.h"
#include "AST/Decl.h"
#include "AST/Expr.h"
#include "AST/Stmt.h"
#include "Utils/color.h"
#include "Utils/StringBuilder.h"

using namespace C2;
using namespace clang;

//#define ANALYSER_DEBUG

#ifdef ANALYSER_DEBUG
#include <iostream>
#define LOG_FUNC std::cerr << ANSI_MAGENTA << __func__ << "()" << ANSI_NORMAL << "\n";
#else
#define LOG_FUNC
#endif

#define MIN(a, b) ((a < b) ? a : b)

#define LHS 0x01
#define RHS 0x02

static void SetConstantFlags(Decl* D, Expr* expr) {
    switch (D->getKind()) {
    case DECL_FUNC:
        expr->setConstant();
        break;
    case DECL_VAR:
        {
            VarDecl* VD = cast<VarDecl>(D);
            QualType T = VD->getType();
            if (T.isConstQualified()) {
                Expr* Init = VD->getInitValue();
                if (Init) {
                    // Copy CTC status of Init Expr
                    expr->setCTC(Init->getCTC());
                }
                expr->setConstant();
                return;
            }
            break;
        }
    case DECL_ENUMVALUE:
        expr->setCTC(CTC_FULL);
        expr->setConstant();
        return;
    case DECL_ALIASTYPE:
    case DECL_STRUCTTYPE:
    case DECL_ENUMTYPE:
    case DECL_FUNCTIONTYPE:
        expr->setConstant();
        break;
    case DECL_ARRAYVALUE:
    case DECL_IMPORT:
    case DECL_LABEL:
        break;
    }
    // TODO needed?
    expr->setCTC(CTC_NONE);
}


FunctionAnalyser::FunctionAnalyser(Scope& scope_,
                                   TypeResolver& typeRes_,
                                   TypeContext& tc,
                                   clang::DiagnosticsEngine& Diags_)
    : scope(scope_)
    , TR(typeRes_)
    , typeContext(tc)
    , EA(Diags_)
    , Diags(Diags_)
    , errors(0)
    , CurrentFunction(0)
    , CurrentVarDecl(0)
    , constDiagID(0)
    , inConstExpr(false)
{
}

unsigned FunctionAnalyser::check(FunctionDecl* func) {
    CurrentFunction = func;
    errors = 0;
    scope.EnterScope(Scope::FnScope | Scope::DeclScope);

    checkFunction(func);

    scope.ExitScope();

    // check labels
    for (LabelsIter iter = labels.begin(); iter != labels.end(); ++iter) {
        LabelDecl* LD = *iter;
        if (LD->getStmt()) {    // have label part
            if (!LD->isUsed()) {
                Diag(LD->getLocation(), diag::warn_unused_label) << LD->DiagName();
            }
        } else {    // only have goto part
            Diag(LD->getLocation(), diag:: err_undeclared_label_use) << LD->DiagName();
        }
        delete LD;
    }
    labels.clear();

    CurrentFunction = 0;
    return errors;
}

unsigned FunctionAnalyser::checkVarInit(VarDecl* V) {
    LOG_FUNC
    CurrentVarDecl = V;
    errors = 0;

    ConstModeSetter cms(*this, diag::err_init_element_not_constant);
    analyseInitExpr(V->getInitValue(), V->getType());
    // TODO if type is array, update type of VarDecl? (add size)

    CurrentVarDecl = 0;
    return errors;
}

unsigned FunctionAnalyser::checkArraySizeExpr(VarDecl* V) {
    LOG_FUNC
    CurrentVarDecl = V;
    errors = 0;

    ConstModeSetter cms(*this, diag::err_init_element_not_constant);
    analyseArrayType(V, V->getType());

    CurrentVarDecl = 0;
    return errors;
}

unsigned FunctionAnalyser::checkEnumValue(EnumConstantDecl* E, llvm::APSInt& nextValue) {
    LOG_FUNC;

    LiteralAnalyser LA(Diags);
    Expr* Init = E->getInitValue();
    if (Init) {
        QualType T = analyseExpr(Init, RHS);
        if (!T.isValid()) return 1;

        if (!Init->isConstant()) {
            Diag(Init->getLocation(), diag::err_expr_not_ice) << 0 << Init->getSourceRange();
            return 1;
        }
        // TODO refactor duplicate code with analyseArraySizeExpr()
        QualType CT = T.getCanonicalType();
        if (!CT.isBuiltinType() || !cast<BuiltinType>(CT)->isInteger()) {
            Diag(Init->getLocation(), diag::err_expr_not_ice) << 0 << Init->getSourceRange();
            return 1;
        }
        assert(Init->getCTC() == CTC_FULL);
        llvm::APSInt V = LA.checkLiterals(Init);
        nextValue = V;
    }
    E->setValue(nextValue);
    nextValue++;
    if (!LA.checkRange(E->getType(), Init, E->getLocation(), E->getValue())) return 1;

    return 0;
}

void FunctionAnalyser::checkFunction(FunctionDecl* func) {
    LOG_FUNC
    // add arguments to new scope
    for (unsigned i=0; i<func->numArgs(); i++) {
        VarDecl* arg = func->getArg(i);
        if (arg->getName() != "") {
            // check that argument names dont clash with globals
            if (!scope.checkScopedSymbol(arg)) {
                errors++;
                continue;
            }
            scope.addScopedSymbol(arg);
        }
    }
    if (errors) return;

    analyseCompoundStmt(func->getBody());
    if (errors) return;

    // check for return statement of return value is required
    QualType rtype = func->getReturnType();
    bool need_rvalue = (rtype.getTypePtr() != BuiltinType::get(BuiltinType::Void));
    if (need_rvalue) {
        CompoundStmt* compound = func->getBody();
        Stmt* lastStmt = compound->getLastStmt();
        if (!lastStmt || lastStmt->getKind() != STMT_RETURN) {
            Diag(compound->getRight(), diag::warn_falloff_nonvoid_function);
        }
    }
}

void FunctionAnalyser::analyseStmt(Stmt* S, bool haveScope) {
    LOG_FUNC
    switch (S->getKind()) {
    case STMT_RETURN:
        analyseReturnStmt(S);
        break;
    case STMT_EXPR:
        analyseStmtExpr(S);
        break;
    case STMT_IF:
        analyseIfStmt(S);
        break;
    case STMT_WHILE:
        analyseWhileStmt(S);
        break;
    case STMT_DO:
        analyseDoStmt(S);
        break;
    case STMT_FOR:
        analyseForStmt(S);
        break;
    case STMT_SWITCH:
        analyseSwitchStmt(S);
        break;
    case STMT_CASE:
    case STMT_DEFAULT:
        assert(0 && "not done here");
        break;
    case STMT_BREAK:
        analyseBreakStmt(S);
        break;
    case STMT_CONTINUE:
        analyseContinueStmt(S);
        break;
    case STMT_LABEL:
        analyseLabelStmt(S);
        break;
    case STMT_GOTO:
        analyseGotoStmt(S);
        break;
    case STMT_COMPOUND:
        if (!haveScope) scope.EnterScope(Scope::DeclScope);
        analyseCompoundStmt(S);
        if (!haveScope) scope.ExitScope();
        break;
    case STMT_DECL:
        analyseDeclStmt(S);
        break;
    }
}

void FunctionAnalyser::analyseCompoundStmt(Stmt* stmt) {
    LOG_FUNC
    assert(CurrentFunction);
    CompoundStmt* compound = cast<CompoundStmt>(stmt);
    const StmtList& stmts = compound->getStmts();
    for (unsigned i=0; i<stmts.size(); i++) {
        analyseStmt(stmts[i]);
    }
}

void FunctionAnalyser::analyseIfStmt(Stmt* stmt) {
    LOG_FUNC
    IfStmt* I = cast<IfStmt>(stmt);

    scope.EnterScope(Scope::DeclScope);
    analyseCondition(I->getCond());

    scope.EnterScope(Scope::DeclScope);
    analyseStmt(I->getThen(), true);
    scope.ExitScope();

    if (I->getElse()) {
        scope.EnterScope(Scope::DeclScope);
        analyseStmt(I->getElse(), true);
        scope.ExitScope();
    }
    scope.ExitScope();
}

void FunctionAnalyser::analyseWhileStmt(Stmt* stmt) {
    LOG_FUNC
    WhileStmt* W = cast<WhileStmt>(stmt);
    scope.EnterScope(Scope::DeclScope);
    analyseStmt(W->getCond());
    scope.EnterScope(Scope::BreakScope | Scope::ContinueScope | Scope::DeclScope | Scope::ControlScope);
    analyseStmt(W->getBody(), true);
    scope.ExitScope();
    scope.ExitScope();
}

void FunctionAnalyser::analyseDoStmt(Stmt* stmt) {
    LOG_FUNC
    DoStmt* D = cast<DoStmt>(stmt);
    scope.EnterScope(Scope::BreakScope | Scope::ContinueScope | Scope::DeclScope);
    analyseStmt(D->getBody());
    scope.ExitScope();
    analyseStmt(D->getCond());
}

void FunctionAnalyser::analyseForStmt(Stmt* stmt) {
    LOG_FUNC
    ForStmt* F = cast<ForStmt>(stmt);
    scope.EnterScope(Scope::BreakScope | Scope::ContinueScope | Scope::DeclScope | Scope::ControlScope);
    if (F->getInit()) analyseStmt(F->getInit());
    if (F->getCond()) {
        QualType CT = analyseExpr(F->getCond(), RHS);
        if (!CT.isScalarType()) {
            StringBuilder buf(64);
            CT.DiagName(buf);
            Diag(F->getCond()->getLocation(), diag::err_typecheck_statement_requires_scalar) << buf;
        }
    }
    if (F->getIncr()) analyseExpr(F->getIncr(), RHS);
    analyseStmt(F->getBody(), true);
    scope.ExitScope();
}

void FunctionAnalyser::analyseSwitchStmt(Stmt* stmt) {
    LOG_FUNC
    SwitchStmt* S = cast<SwitchStmt>(stmt);

    scope.EnterScope(Scope::DeclScope);
    analyseCondition(S->getCond());

    const StmtList& Cases = S->getCases();
    Stmt* defaultStmt = 0;
    scope.EnterScope(Scope::BreakScope | Scope::SwitchScope);
    for (unsigned i=0; i<Cases.size(); i++) {
        Stmt* C = Cases[i];
        switch (C->getKind()) {
        case STMT_CASE:
            analyseCaseStmt(C);
            break;
        case STMT_DEFAULT:
            if (defaultStmt) {
                Diag(C->getLocation(), diag::err_multiple_default_labels_defined);
                Diag(defaultStmt->getLocation(), diag::note_duplicate_case_prev);
            } else {
                defaultStmt = C;
            }
            analyseDefaultStmt(C);
            break;
        default:
            assert(0);
        }
    }

    QualType QT = getConditionType(S->getCond());
    if (const EnumType* ET = dyncast<EnumType>(QT)) {
        checkEnumCases(S, ET);
    }

    scope.ExitScope();
    scope.ExitScope();
}

void FunctionAnalyser::analyseBreakStmt(Stmt* stmt) {
    LOG_FUNC
    if (!scope.allowBreak()) {
        BreakStmt* B = cast<BreakStmt>(stmt);
        Diag(B->getLocation(), diag::err_break_not_in_loop_or_switch);
    }
}

void FunctionAnalyser::analyseContinueStmt(Stmt* stmt) {
    LOG_FUNC
    if (!scope.allowContinue()) {
        ContinueStmt* C = cast<ContinueStmt>(stmt);
        Diag(C->getLocation(), diag::err_continue_not_in_loop);
    }
}

void FunctionAnalyser::analyseLabelStmt(Stmt* S) {
    LOG_FUNC
    LabelStmt* L = cast<LabelStmt>(S);

    LabelDecl* LD = LookupOrCreateLabel(L->getName(), L->getLocation());
    if (LD->getStmt()) {
        Diag(L->getLocation(), diag::err_redefinition_of_label) <<  LD->DiagName();
        Diag(LD->getLocation(), diag::note_previous_definition);
    } else {
        LD->setStmt(L);
        LD->setLocation(L->getLocation());
    }

    analyseStmt(L->getSubStmt());
    // substmt cannot be declaration

    if (isa<DeclStmt>(L->getSubStmt())) {
        Diag(L->getSubStmt()->getLocation(), diag::err_decl_after_label);
    }
}

void FunctionAnalyser::analyseGotoStmt(Stmt* S) {
    LOG_FUNC
    GotoStmt* G = cast<GotoStmt>(S);
    LabelDecl* LD = LookupOrCreateLabel(G->getName(), G->getLocation());
    LD->setUsed();
}

void FunctionAnalyser::analyseCaseStmt(Stmt* stmt) {
    LOG_FUNC
    CaseStmt* C = cast<CaseStmt>(stmt);
    analyseExpr(C->getCond(), RHS);
    const StmtList& stmts = C->getStmts();
    for (unsigned i=0; i<stmts.size(); i++) {
        analyseStmt(stmts[i]);
    }
}

void FunctionAnalyser::analyseDefaultStmt(Stmt* stmt) {
    LOG_FUNC
    DefaultStmt* D = cast<DefaultStmt>(stmt);
    const StmtList& stmts = D->getStmts();
    for (unsigned i=0; i<stmts.size(); i++) {
        analyseStmt(stmts[i]);
    }
}

void FunctionAnalyser::analyseReturnStmt(Stmt* stmt) {
    LOG_FUNC
    ReturnStmt* ret = cast<ReturnStmt>(stmt);
    Expr* value = ret->getExpr();
    QualType rtype = CurrentFunction->getReturnType();
    bool no_rvalue = (rtype.getTypePtr() == Type::Void());
    if (value) {
        QualType type = analyseExpr(value, RHS);
        if (no_rvalue) {
            Diag(ret->getLocation(), diag::ext_return_has_expr) << CurrentFunction->getName() << 0
                << value->getSourceRange();
        } else {
            if (type.isValid()) {
                EA.check(rtype, value);
            }
        }
    } else {
        if (!no_rvalue) {
            Diag(ret->getLocation(), diag::ext_return_missing_expr) << CurrentFunction->getName() << 0;
        }
    }
}

void FunctionAnalyser::analyseDeclStmt(Stmt* stmt) {
    LOG_FUNC
    DeclStmt* DS = cast<DeclStmt>(stmt);
    VarDecl* decl = DS->getDecl();

    bool haveError = false;
    QualType Q = TR.resolveType(decl->getType(), decl->isPublic());
    if (Q.isValid()) {
        decl->setType(Q);

        if (Q.isArrayType()) {
            // TODO use Helper-function to get ArrayType (might be AliasType)
            ArrayType* AT = cast<ArrayType>(Q.getTypePtr());
            Expr* sizeExpr = AT->getSizeExpr();
            if (sizeExpr) {
                analyseArraySizeExpr(AT);
                if (sizeExpr->getCTC() != CTC_FULL && decl->getInitValue()) {
                    Diag(decl->getLocation(), diag::err_vla_with_init_value) << decl->getInitValue()->getLocation();
                    haveError = true;
                }
            } else {
                if (AT->isIncremental()) {
                    Diag(decl->getLocation(), diag::err_incremental_array_function_scope);
                    haveError = true;
                } else if (!decl->getInitValue()) {
                    Diag(decl->getLocation(), diag::err_typecheck_incomplete_array_needs_initializer);
                    haveError = true;
                }
            }
        }
    }

    // check name
    if (!scope.checkScopedSymbol(decl)) return;

    // check initial value
    Expr* initialValue = decl->getInitValue();
    if (initialValue && !haveError) {
        CurrentVarDecl = decl;
        analyseInitExpr(initialValue, decl->getType());
        CurrentVarDecl = 0;
    }

    if (Q.isConstQualified() && !initialValue) {
        Diag(decl->getLocation(), diag::err_uninitialized_const_var) << decl->getName();
    }
    scope.addScopedSymbol(decl);
}

void FunctionAnalyser::analyseCondition(Stmt* stmt) {
    LOG_FUNC

    if (isa<DeclStmt>(stmt)) {
       analyseDeclStmt(stmt);
    } else {
        assert(isa<Expr>(stmt));
        Expr* E = cast<Expr>(stmt);
        QualType Q1 = analyseExpr(E, RHS);
        if (Q1.isValid()) {
            EA.check(Type::Bool(), E);
        }
    }
}

void FunctionAnalyser::analyseStmtExpr(Stmt* stmt) {
    LOG_FUNC
    Expr* expr = cast<Expr>(stmt);
    analyseExpr(expr, 0);
}

C2::QualType FunctionAnalyser::analyseExpr(Expr* expr, unsigned side) {
    LOG_FUNC
    if (side & LHS) {
        if (!checkAssignee(expr)) return QualType();
    }

    switch (expr->getKind()) {
    case EXPR_INTEGER_LITERAL:
        return analyseIntegerLiteral(expr);
    case EXPR_FLOAT_LITERAL:
        // For now always return type float
        expr->setType(Type::Float32());
        return Type::Float32();
    case EXPR_BOOL_LITERAL:
        expr->setType(Type::Bool());
        return Type::Bool();
    case EXPR_CHAR_LITERAL:
        expr->setType(Type::Int8());
        return Type::Int8();
    case EXPR_STRING_LITERAL:
        {
            // return type: 'const char*'
            QualType Q = typeContext.getPointerType(Type::Int8());
            Q.addConst();
            if (!Q->hasCanonicalType()) Q->setCanonicalType(Q);
            expr->setType(Q);
            return Q;
        }
    case EXPR_NIL:
        {
            QualType Q = typeContext.getPointerType(Type::Void());
            if (!Q->hasCanonicalType()) Q->setCanonicalType(Q);
            expr->setType(Q);
            return Q;
        }
    case EXPR_CALL:
        return analyseCall(expr);
    case EXPR_IDENTIFIER:
        {
            IdentifierExpr* id = cast<IdentifierExpr>(expr);
            Decl* D = analyseIdentifier(id);
            if (!D) break;
            if (D == CurrentVarDecl) {
                Diag(id->getLocation(), diag::err_var_self_init) << D->getName();
                return QualType();
            }
            if (side & LHS) checkDeclAssignment(D, expr);
            switch (D->getKind()) {
            case DECL_FUNC:
                expr->setConstant();
                break;
            case DECL_VAR:
                break;
            case DECL_ENUMVALUE:
                expr->setCTC(CTC_FULL);
                expr->setConstant();
                break;
            case DECL_ALIASTYPE:
            case DECL_STRUCTTYPE:
            case DECL_ENUMTYPE:
            case DECL_FUNCTIONTYPE:
                Diag(id->getLocation(), diag::err_unexpected_typedef) << id->getName();
                expr->setConstant();
                break;
            case DECL_ARRAYVALUE:
                break;
            case DECL_IMPORT:
                expr->setConstant();
                break;
            case DECL_LABEL:
                assert(0 && "TODO");
                break;
            }
            if (side & RHS) D->setUsed();
            QualType T = D->getType();
            expr->setType(T);
            return T;
        }
        break;
    case EXPR_INITLIST:
    case EXPR_DESIGNATOR_INIT:
    case EXPR_TYPE:
        // dont handle here
        break;
    case EXPR_BINOP:
        return analyseBinaryOperator(expr, side);
    case EXPR_CONDOP:
        return analyseConditionalOperator(expr);
    case EXPR_UNARYOP:
        return analyseUnaryOperator(expr, side);
    case EXPR_BUILTIN:
        return analyseBuiltinExpr(expr);
    case EXPR_ARRAYSUBSCRIPT:
        return analyseArraySubscript(expr, side);
    case EXPR_MEMBER:
        return analyseMemberExpr(expr, side);
    case EXPR_PAREN:
        return analyseParenExpr(expr);
    case EXPR_BITOFFSET:
        assert(0 && "should not happen");
        break;
    }
    return QualType();
}

void FunctionAnalyser::analyseInitExpr(Expr* expr, QualType expectedType) {
    LOG_FUNC
    InitListExpr* ILE = dyncast<InitListExpr>(expr);
    // FIXME: expectedType has no canonicalType yet!
    const ArrayType* AT = dyncast<ArrayType>(expectedType.getCanonicalType());
    if (AT) {
        const QualType ET = AT->getElementType();
        bool isCharArray = (ET == Type::Int8() || ET == Type::UInt8());

        if (isCharArray) {
            if (!ILE && !isa<StringLiteral>(expr)) {
                Diag(expr->getLocation(), diag::err_array_init_not_init_list) << 1;
                return;
            }
        } else {
            if (!ILE) {
                Diag(expr->getLocation(), diag::err_array_init_not_init_list) << 0;
                return;
            }
        }
    }
    if (ILE) {
        analyseInitList(ILE, expectedType);
    } else if (isa<DesignatedInitExpr>(expr)) {
        analyseDesignatorInitExpr(expr, expectedType);
    } else {
        QualType Q = analyseExpr(expr, RHS);
        if (Q.isValid()) {
            if (inConstExpr && !expr->isConstant()) {
                assert(constDiagID);
                Diag(expr->getLocation(), constDiagID) << expr->getSourceRange();
            } else {
                EA.check(expectedType, expr);
            }
            if (AT && AT->getSizeExpr()) {
                // it should be char array type already and expr is string literal
                assert(isa<StringLiteral>(expr));
                const StringLiteral* S = cast<StringLiteral>(expr);
                if (S->getByteLength() > AT->getSize().getZExtValue()) {
                    Diag(S->getLocation(), diag::err_initializer_string_for_char_array_too_long) << S->getSourceRange();
                    return;
                }
            }
        }
    }
}

void FunctionAnalyser::analyseInitList(InitListExpr* expr, QualType Q) {
    LOG_FUNC
    const ExprList& values = expr->getValues();
    bool haveDesignators = false;
    bool haveErrors = false;
    if (Q.isArrayType()) {
        // TODO use helper function
        ArrayType* AT = cast<ArrayType>(Q.getCanonicalType().getTypePtr());
        QualType ET = AT->getElementType();
        bool constant = true;
        for (unsigned i=0; i<values.size(); i++) {
            analyseInitExpr(values[i], ET);
            if (DesignatedInitExpr* D = cast<DesignatedInitExpr>(values[i])) {
                haveDesignators = true;
                if (D->getDesignatorKind() != DesignatedInitExpr::ARRAY_DESIGNATOR) {
                    StringBuilder buf;
                    Q.DiagName(buf);
                    Diag(D->getLocation(), diag::err_field_designator_non_aggr) << 0 << buf;
                    haveErrors = true;
                    continue;
                }
            }
            if (!values[i]->isConstant()) constant = false;
        }
        if (constant) expr->setConstant();
        if (haveDesignators) expr->setDesignators();
        // determine real array size
        llvm::APInt initSize(64, 0, false);
        if (haveDesignators && !haveErrors) {
            int64_t arraySize = -1;
            if (AT->getSizeExpr()) {    // size determined by expr
                arraySize = AT->getSize().getZExtValue();
            } //else, size determined by designators
            checkArrayDesignators(expr, &arraySize);
            initSize = arraySize;
        } else {
            if (AT->getSizeExpr()) {    // size determined by expr
                initSize = AT->getSize();
                uint64_t arraySize = AT->getSize().getZExtValue();
                if (values.size() > arraySize) {
                    int firstExceed = AT->getSize().getZExtValue();
                    Diag(values[firstExceed]->getLocation(), diag::err_excess_initializers) << 0;
                }
            } else {    // size determined from #elems in initializer list
                initSize = values.size();
            }
        }
        AT->setSize(initSize);
        expr->setType(Q);
    } else if (Q.isStructType()) {
        expr->setType(Q);
        // TODO use helper function
        StructType* TT = cast<StructType>(Q.getCanonicalType().getTypePtr());
        StructTypeDecl* STD = TT->getDecl();
        assert(STD->isStruct() && "TEMP only support structs for now");
        bool constant = true;
        // ether init whole struct with field designators, or don't use any (no mixing allowed)
        if (!values.empty() && isa<DesignatedInitExpr>(values[0])) {
            haveDesignators = true;
        }
        // TODO cleanup this code (after unit-tests) Split into field-designator / non-designator init
        typedef std::vector<Expr*> Fields;
        Fields fields;
        fields.resize(STD->numMembers());
        for (unsigned i=0; i<values.size(); i++) {
            if (i >= STD->numMembers()) {
                // note: 0 for array, 2 for scalar, 3 for union, 4 for structs
                Diag(values[STD->numMembers()]->getLocation(), diag::err_excess_initializers)
                    << 4;
                errors++;
                return;
            }
            if (DesignatedInitExpr* D = dyncast<DesignatedInitExpr>(values[i])) {
                if (D->getDesignatorKind() != DesignatedInitExpr::FIELD_DESIGNATOR) {
                    StringBuilder buf;
                    Q.DiagName(buf);
                    Diag(D->getLocation(), diag::err_array_designator_non_array) << buf;
                    haveErrors = true;
                    return;
                }
                int memberIndex = STD->findIndex(D->getField());
                if (memberIndex == -1) {
                    // TODO use Helper to add surrounding ''
                    StringBuilder fname(MAX_LEN_VARNAME);
                    fname << '\'' << D->getField() << '\'';
                    StringBuilder tname(MAX_LEN_TYPENAME);
                    Q.DiagName(tname);
                    Diag(D->getLocation(), diag::err_field_designator_unknown) << fname << tname;
                    continue;
                }
                Expr* existing = fields[memberIndex];
                if (existing) {
                    StringBuilder fname(MAX_LEN_VARNAME);
                    fname << '\'' << D->getField() << '\'';
                    Diag(D->getLocation(), diag::err_duplicate_field_init) << fname;
                    Diag(existing->getLocation(), diag::note_previous_initializer) << 0 << 0;
                    continue;
                }
                fields[memberIndex] = values[i];
                Decl* member = STD->getMember(memberIndex);
                assert(member);
                VarDecl* VD = dyncast<VarDecl>(STD->getMember(i));
                assert(VD && "TEMP don't support sub-struct member inits");
                analyseInitExpr(D->getInitValue(), VD->getType());
            } else {
                VarDecl* VD = dyncast<VarDecl>(STD->getMember(i));
                assert(VD && "TEMP don't support sub-struct member inits");
                analyseInitExpr(values[i], VD->getType());
                if (!values[i]->isConstant()) constant = false;
            }
            if (isa<DesignatedInitExpr>(values[i]) != haveDesignators) {
                Diag(values[i]->getLocation(), diag::err_mixed_field_designator);
                return;
            }

        }
        if (constant) expr->setConstant();
    } else {
        // TODO always give error like case 1?
        // only allow 1
        switch (values.size()) {
        case 0:
            fprintf(stderr, "TODO ERROR: scalar initializer cannot be empty\n");
            errors++;
            break;
        case 1:
        {
            StringBuilder buf;
            Q.DiagName(buf);
            Diag(expr->getLocation(), diag::err_invalid_initlist_init) << buf << expr->getSourceRange();
            break;
        }
        default:
            Diag(values[1]->getLocation(), diag::err_excess_initializers) << 2;
            errors++;
            break;
        }
        return;
    }
}

void FunctionAnalyser::analyseDesignatorInitExpr(Expr* expr, QualType expectedType) {
    LOG_FUNC
    DesignatedInitExpr* D = cast<DesignatedInitExpr>(expr);
    if (D->getDesignatorKind() == DesignatedInitExpr::ARRAY_DESIGNATOR) {
        Expr* Desig = D->getDesignator();
        QualType DT = analyseExpr(Desig, RHS);
        if (DT.isValid()) {
            if (!Desig->isConstant()) {
                assert(constDiagID);
                Diag(Desig->getLocation(), constDiagID) << Desig->getSourceRange();
            } else {
                if (!Desig->getType().isIntegerType()) {
                    Diag(Desig->getLocation(), diag::err_typecheck_subscript_not_integer) << Desig->getSourceRange();
                } else {
                    LiteralAnalyser LA(Diags);
                    llvm::APSInt V = LA.checkLiterals(Desig);
                    if (V.isSigned() && V.isNegative()) {
                        Diag(Desig->getLocation(), diag::err_array_designator_negative) << V.toString(10) << Desig->getSourceRange();
                    } else {
                        D->setIndex(V);
                    }
                }
            }
        }
    } else {
        // cannot check here, need structtype
    }

    analyseInitExpr(D->getInitValue(), expectedType);
    if (D->getInitValue()->isConstant()) D->setConstant();

    D->setType(expectedType);
}

void FunctionAnalyser::analyseSizeofExpr(Expr* expr) {
    switch (expr->getKind()) {
    case EXPR_INTEGER_LITERAL:
    case EXPR_FLOAT_LITERAL:
    case EXPR_BOOL_LITERAL:
    case EXPR_CHAR_LITERAL:
    case EXPR_STRING_LITERAL:
    case EXPR_NIL:
        // ok
        return;
    case EXPR_CALL:
        // not allowed
        assert(0 && "TODO good error");
        return;
    case EXPR_IDENTIFIER:
    {
        // TODO extract to function (duplicated code)
        IdentifierExpr* id = cast<IdentifierExpr>(expr);
        Decl* D = analyseIdentifier(id);
        if (!D) return;
        switch (D->getKind()) {
        case DECL_FUNC:
            // Q: allow?
        case DECL_VAR:
        case DECL_ENUMVALUE:
        case DECL_ALIASTYPE:
        case DECL_STRUCTTYPE:
        case DECL_ENUMTYPE:
        case DECL_FUNCTIONTYPE:
            // ok
            return;
        case DECL_ARRAYVALUE:
            assert(0 && "should not happen");
            break;
        case DECL_IMPORT:
            Diag(id->getLocation(), diag::err_is_a_module) << id->getName();
            return;
        case DECL_LABEL:
            assert(0 && "TODO");
            break;
        }
        break;
    }
    case EXPR_INITLIST:
    case EXPR_DESIGNATOR_INIT:
        assert(0 && "TODO good error");
        return;
    case EXPR_TYPE:
        // ok
        return;
    case EXPR_BINOP:
        // not allowed
        assert(0 && "TODO good error");
    case EXPR_CONDOP:
        assert(0 && "TODO allow?");
        return;
    case EXPR_UNARYOP:
        // some allowed (&)?
        // TODO
        return;
    case EXPR_BUILTIN:
        assert(0 && "should not happen");
        return;
    case EXPR_ARRAYSUBSCRIPT:
    case EXPR_MEMBER:
        // allowed
        assert(0 && "TODO");
        return;
    case EXPR_PAREN:
        analyseSizeofExpr(cast<ParenExpr>(expr)->getExpr());
        return;
    case EXPR_BITOFFSET:
        assert(0 && "should not happen");
        break;
    }

}

// sets ArrayType sizes recursively if sizeExpr is constant
// NOTE: doesn't check any initExpr itself
void FunctionAnalyser::analyseArrayType(VarDecl* V, QualType T) {
    LOG_FUNC
    // TODO V not needed? (or move more diags here from checkVarInits)
    if (!T.isArrayType()) return;

    switch (T->getTypeClass()) {
    case TC_BUILTIN:
        assert(0 && "should not happen");
        break;
    case TC_POINTER:
        analyseArrayType(V, cast<PointerType>(T)->getPointeeType());
        break;
    case TC_ARRAY:
        {
            ArrayType* AT = cast<ArrayType>(T.getTypePtr());
            analyseArraySizeExpr(AT);
            analyseArrayType(V, AT->getElementType());
            // generate empty InitList for Incremental Array decls
            if (AT->isIncremental() && V->getInitValue() == 0) {
                ExprList vals;
                V->setInitValue(new InitListExpr(SourceLocation(), SourceLocation(), vals));
            }
            break;
        }
    case TC_UNRESOLVED:
        assert(0 && "should not happen");
        break;
    case TC_ALIAS:
        analyseArrayType(V, cast<AliasType>(T)->getRefType());
        break;
    case TC_STRUCT:
    case TC_ENUM:
    case TC_FUNCTION:
        assert(0 && "should not happen");
        break;
    case TC_PACKAGE:
        assert(0 && "TBD");
        break;
    }
}

void FunctionAnalyser::analyseArraySizeExpr(ArrayType* AT) {
    LOG_FUNC
    Expr* E = AT->getSizeExpr();
    if (!E) return;

    QualType T = analyseExpr(E, RHS);
    if (!T.isValid()) return;

    if (inConstExpr && !E->isConstant()) {
        Diag(E->getLocation(), diag::err_vla_decl_in_file_scope) << E->getSourceRange();
        errors++;
        return;
    }
    // check if type is integer
    QualType CT = T.getCanonicalType();
    if (!CT.isBuiltinType() || !cast<BuiltinType>(CT)->isInteger()) {
        StringBuilder buf;
        T.DiagName(buf);
        Diag(E->getLocation(), diag::err_array_size_non_int) << buf << E->getSourceRange();
        errors++;
        return;
    }
    // check if negative
    if (E->getCTC() == CTC_FULL) {
        LiteralAnalyser LA(Diags);
        llvm::APSInt Result = LA.checkLiterals(E);
        if (Result.isSigned() && Result.isNegative()) {
            Diag(E->getLocation(), diag::err_typecheck_negative_array_size) << E->getSourceRange();
            errors++;
        } else {
            AT->setSize(Result);
        }
    }
}

static ExprCTC combineCtc(Expr* Result, const Expr* L, const Expr* R) {
    const ExprCTC left =  L->getCTC();
    const ExprCTC right = R->getCTC();
    switch (left + right) {
    case 0:
        Result->setCTC(CTC_NONE);
        return CTC_NONE;
    case 1:
    case 2:
    case 3:
        Result->setCTC(CTC_PARTIAL);
        return CTC_PARTIAL;
    case 4:
        Result->setCTC(CTC_FULL);
        return CTC_FULL;
    }
    assert(0 && "should not come here");
    return CTC_NONE;
}

QualType FunctionAnalyser::analyseIntegerLiteral(Expr* expr) {
    IntegerLiteral* I = cast<IntegerLiteral>(expr);
    // Fit smallest Type: int32 > uint32 > int64 > uint64
    // TODO unsigned types

    // TEMP for now assume signed
    // Q: we can determine size, but don't know if we need signed/unsigned
    //unsigned numbits = I->Value.getMinSignedBits();  // signed
    unsigned numbits = I->Value.getActiveBits();   // unsigned
    //if (numbits <= 8) return Type::Int8();
    //if (numbits <= 16) return Type::Int16();
    expr->setType(Type::Int32());
    if (numbits <= 32) return Type::Int32();
    expr->setType(Type::Int64());
    return Type::Int64();
}

static bool isConstantBitOffset(const Expr* E) {
    if (const ArraySubscriptExpr* A = dyncast<ArraySubscriptExpr>(E)) {
        if (const BitOffsetExpr* B = dyncast<BitOffsetExpr>(A->getIndex())) {
            return B->isConstant();
        }
    }
    return false;
}

QualType FunctionAnalyser::analyseBinaryOperator(Expr* expr, unsigned side) {
    LOG_FUNC
    BinaryOperator* binop = cast<BinaryOperator>(expr);
    QualType TLeft, TRight;
    Expr* Left = binop->getLHS();
    Expr* Right = binop->getRHS();

    switch (binop->getOpcode()) {
    case BO_PtrMemD:
    case BO_PtrMemI:
        assert(0 && "unhandled binary operator type");
        break;
    case BO_Mul:
    case BO_Div:
    case BO_Rem:
    case BO_Add:
    case BO_Sub:
    case BO_Shl:
    case BO_Shr:
        // RHS, RHS
        TLeft = analyseExpr(Left, RHS);
        TRight = analyseExpr(Right, RHS);
        combineCtc(expr, Left, Right);
        if (Left->isConstant() && Right->isConstant()) expr->setConstant();
        break;
    case BO_LE:
    case BO_LT:
    case BO_GE:
    case BO_GT:
    case BO_NE:
    case BO_EQ:
        // RHS, RHS
        TLeft = analyseExpr(Left, RHS);
        TRight = analyseExpr(Right, RHS);
        // NOTE: CTC is never full, because value is not interting, only type
        if (Left->isConstant() && Right->isConstant()) expr->setConstant();
        break;
    case BO_And:
    case BO_Xor:
    case BO_Or:
    case BO_LAnd:
    case BO_LOr:
        // TODO check if cast of SubExpr is ok
        // RHS, RHS
        TLeft = analyseExpr(Left, RHS);
        TRight = analyseExpr(Right, RHS);
        combineCtc(expr, Left, Right);
        if (Left->isConstant() && Right->isConstant()) expr->setConstant();
        break;
    case BO_Assign:
        // LHS, RHS
        TLeft = analyseExpr(Left, side | LHS);
        TRight = analyseExpr(Right, RHS);
        break;
    case BO_MulAssign:
    case BO_DivAssign:
    case BO_RemAssign:
    case BO_AddAssign:
    case BO_SubAssign:
    case BO_ShlAssign:
    case BO_ShrAssign:
    case BO_AndAssign:
    case BO_XorAssign:
    case BO_OrAssign:
        // LHS|RHS, RHS
        TLeft = analyseExpr(Left, LHS | RHS);
        TRight = analyseExpr(Right, RHS);
        break;
    case BO_Comma:
        assert(0 && "unhandled binary operator type");
        break;
    }
    if (TLeft.isNull() || TRight.isNull()) return QualType();

    // determine Result type
    QualType Result;
    switch (binop->getOpcode()) {
    case BO_PtrMemD:
    case BO_PtrMemI:
        assert(0 && "unhandled binary operator type");
        break;
    case BO_Mul:
    case BO_Div:
    case BO_Rem:
    case BO_Add:
    case BO_Sub:
        // TODO return largest witdth of left/right (long*short -> long)
        // TODO apply UsualArithmeticConversions() to L + R
        // TEMP for now just return Right side
        // TEMP use UnaryConversion
        Result = UsualUnaryConversions(Left);
        UsualUnaryConversions(Right);
        break;
    case BO_Shl:
    case BO_Shr:
        Result = TLeft;
        break;
    case BO_LE:
    case BO_LT:
    case BO_GE:
    case BO_GT:
    case BO_NE:
    case BO_EQ:
        Result = Type::Bool();
        break;
    case BO_And:
    case BO_Xor:
    case BO_Or:
        Result = TLeft;
        break;
    case BO_LAnd:
    case BO_LOr:
        Result = Type::Bool();
        break;
    case BO_Assign:
    {
        assert(TRight.isValid());
        // special case for a[4:2] = b
        if (isConstantBitOffset(Left) && Right->isConstant()) {
            LiteralAnalyser LA(Diags);
            LA.checkBitOffset(Left, Right);
        } else {
            EA.check(TLeft, Right);
        }
        checkAssignment(Left, TLeft);
        Result = TLeft;
        break;
    }
    case BO_MulAssign:
    case BO_DivAssign:
    case BO_RemAssign:
    case BO_AddAssign:
    case BO_SubAssign:
    case BO_ShlAssign:
    case BO_ShrAssign:
    case BO_AndAssign:
    case BO_XorAssign:
    case BO_OrAssign:
        checkAssignment(Left, TLeft);
        Result = TLeft;
        break;
    case BO_Comma:
        assert(0 && "unhandled binary operator type");
        break;
    }
    expr->setType(Result);
    return Result;
}

QualType FunctionAnalyser::analyseConditionalOperator(Expr* expr) {
    LOG_FUNC
    ConditionalOperator* condop = cast<ConditionalOperator>(expr);
    analyseExpr(condop->getCond(), RHS);
    // check if Condition can be casted to bool
    EA.check(Type::Bool(), condop->getCond());
    QualType TLeft = analyseExpr(condop->getLHS(), RHS);
    analyseExpr(condop->getRHS(), RHS);
    expr->setCTC(CTC_PARTIAL);  // always set to Partial for ExprTypeAnalyser
    return TLeft;
}

QualType FunctionAnalyser::analyseUnaryOperator(Expr* expr, unsigned side) {
    LOG_FUNC
    UnaryOperator* unaryop = cast<UnaryOperator>(expr);
    Expr* SubExpr = unaryop->getExpr();
    QualType LType;
    // TODO cleanup, always break and do common stuff at end
    switch (unaryop->getOpcode()) {
    case UO_PostInc:
    case UO_PostDec:
    case UO_PreInc:
    case UO_PreDec:
        LType = analyseExpr(SubExpr, side | LHS);
        if (LType.isNull()) return 0;
        checkAssignment(SubExpr, LType);
        expr->setType(LType);
        break;
    case UO_AddrOf:
        {
            LType = analyseExpr(SubExpr, side | RHS);
            if (LType.isNull()) return 0;
            QualType Q = typeContext.getPointerType(LType);
            expr->setType(Q);
            expr->setConstant();
            TR.resolveCanonicals(0, Q, true);
            return Q;
        }
    case UO_Deref:
        LType = analyseExpr(SubExpr, side | RHS);
        if (LType.isNull()) return 0;
        if (!LType.isPointerType()) {
            char typeName[MAX_LEN_TYPENAME];
            StringBuilder buf(MAX_LEN_TYPENAME, typeName);
            LType.DiagName(buf);
            Diag(unaryop->getOpLoc(), diag::err_typecheck_indirection_requires_pointer)
                << buf;
            return 0;
        } else {
            // TEMP use CanonicalType to avoid Unresolved types etc
            QualType Q = LType.getCanonicalType();
            const PointerType* P = cast<PointerType>(Q);
            return P->getPointeeType();
        }
        break;
    case UO_Plus:
    case UO_Minus:
    case UO_Not:
        LType = analyseExpr(SubExpr, side | RHS);
        unaryop->setCTC(SubExpr->getCTC());
        if (SubExpr->isConstant()) unaryop->setConstant();
        if (LType.isNull()) return 0;
        expr->setType(UsualUnaryConversions(SubExpr));
        break;
    case UO_LNot:
        // TODO first cast expr to bool, then invert here, return type bool
        // TODO extract to function
        LType = analyseExpr(SubExpr, side | RHS);
        // TODO check conversion to bool here!!
        unaryop->setCTC(SubExpr->getCTC());
        // Also set type?
        if (LType.isNull()) return 0;
        //UsualUnaryConversions(SubExpr);
        LType = Type::Bool();
        expr->setType(LType);
        break;
    default:
        assert(0 && "TODO");
        break;
    }
    return LType;
}

QualType FunctionAnalyser::analyseBuiltinExpr(Expr* expr) {
    LOG_FUNC
    BuiltinExpr* func = cast<BuiltinExpr>(expr);
    expr->setType(Type::UInt32());
    if (func->isSizeof()) { // sizeof()
        analyseSizeofExpr(func->getExpr());
    } else { // elemsof()
        // TODO proper checking
        analyseExpr(func->getExpr(), RHS);
        Expr* E = func->getExpr();
        IdentifierExpr* I = cast<IdentifierExpr>(E);
        Decl* D = I->getDecl();
        // should be VarDecl(for array/enum) or TypeDecl(array/enum)
        switch (D->getKind()) {
        case DECL_FUNC:
             fprintf(stderr, "TODO Function err\n");
            // ERROR
            break;
        case DECL_VAR:
            {
                VarDecl* VD = cast<VarDecl>(D);
                QualType Q = VD->getType();
                // TODO also allow elemsof for EnumType
                if (!Q.isArrayType()) {
                    StringBuilder msg;
                    Q.DiagName(msg);
                    Diag(I->getLocation(), diag::err_invalid_elemsof_type)
                        << msg;
                }
                return Type::UInt32();
            }
        case DECL_ENUMVALUE:
            // ERROR
            break;
        case DECL_ALIASTYPE:
        case DECL_STRUCTTYPE:
        case DECL_ENUMTYPE:
        case DECL_FUNCTIONTYPE:
            assert(0 && "TODO");
            break;
        case DECL_ARRAYVALUE:
        case DECL_IMPORT:
        case DECL_LABEL:
            assert(0);
            break;
        }
    }
    return Type::UInt32();
}

QualType FunctionAnalyser::analyseArraySubscript(Expr* expr, unsigned side) {
    LOG_FUNC
    ArraySubscriptExpr* sub = cast<ArraySubscriptExpr>(expr);
    QualType LType = analyseExpr(sub->getBase(), RHS);
    if (LType.isNull()) return 0;

    if (BitOffsetExpr* BO = dyncast<BitOffsetExpr>(sub->getIndex())) {
        if (side & LHS) {
            Diag(BO->getLocation(), diag::err_bitoffset_lhs) << expr->getSourceRange();
            return 0;
        }
        QualType T = analyseBitOffsetExpr(sub->getIndex(), LType, sub->getLocation());
        expr->setType(T);
        combineCtc(expr, sub->getBase(), sub->getIndex());
        // dont analyse partial BitOffset expressions per part
        if (expr->getCTC() == CTC_PARTIAL) expr->setCTC(CTC_NONE);
        return T;
    }

    analyseExpr(sub->getIndex(), RHS);

    if (!LType.isSubscriptable()) {
        Diag(expr->getLocation(), diag::err_typecheck_subscript);
        return 0;
    }

    QualType Result;
    if (isa<PointerType>(LType)) {
        Result = cast<PointerType>(LType)->getPointeeType();
    } else if (isa<ArrayType>(LType)) {
        Result = cast<ArrayType>(LType)->getElementType();
    }
    expr->setType(Result);
    return Result;
}

QualType FunctionAnalyser::analyseMemberExpr(Expr* expr, unsigned side) {
    LOG_FUNC
    MemberExpr* M = cast<MemberExpr>(expr);
    const std::string& member = M->getMemberName();
    SourceLocation memberLoc = M->getMemberLoc();

    // we dont know what we're looking at here, it could be:
    // mod.type
    // mod.var
    // mod.func
    // var(Type=struct>.member
    // var[index].member
    // var->member
    QualType LType = analyseExpr(M->getBase(), RHS);
    if (!LType.isValid()) return QualType();

    if (isa<ModuleType>(LType)) {
        M->setModulePrefix(true);
        ModuleType* PT = cast<ModuleType>(LType.getTypePtr());
        Decl* D = scope.findSymbolInModule(member, memberLoc, PT->getModule());
        if (D) {
            if (side & RHS) D->setUsed();
            M->setDecl(D);
            SetConstantFlags(D, M);
            QualType Q = D->getType();
            expr->setType(Q);
            return Q;
        }
    } else {
        M->setModulePrefix(false);
        // dereference pointer
        if (LType.isPointerType()) {
            // TODO use function to get elementPtr (could be alias type)
            const PointerType* PT = cast<PointerType>(LType.getTypePtr());
            LType = PT->getPointeeType();
        }
        QualType S = getStructType(LType);
        if (!S.isValid()) {
            StringBuilder buf(MAX_LEN_TYPENAME);
            LType.DiagName(buf);
            Diag(M->getLocation(), diag::err_typecheck_member_reference_struct_union)
                << buf << M->getSourceRange() << memberLoc;
            return QualType();
        }
        return analyseStructMember(S, M, side);

    }
    return QualType();
}

QualType FunctionAnalyser::analyseStructMember(QualType T, MemberExpr* M, unsigned side) {
    LOG_FUNC
    const StructType* ST = cast<StructType>(T);
    const StructTypeDecl* S = ST->getDecl();
    Decl* match = S->find(M->getMemberName());
    if (match) {
        if (side & RHS) match->setUsed();
        M->setDecl(match);
        M->setType(match->getType());
        return match->getType();
    }
    char temp1[MAX_LEN_TYPENAME];
    StringBuilder buf1(MAX_LEN_TYPENAME, temp1);
    T->DiagName(buf1);

    char temp2[MAX_LEN_VARNAME];
    StringBuilder buf2(MAX_LEN_VARNAME, temp2);
    buf2 << '\'' << M->getMemberName() << '\'';
    Diag(M->getMemberLoc(), diag::err_no_member) << temp2 << temp1;
    return QualType();
}

QualType FunctionAnalyser::analyseParenExpr(Expr* expr) {
    LOG_FUNC
    ParenExpr* P = cast<ParenExpr>(expr);
    QualType Q = analyseExpr(P->getExpr(), RHS);
    expr->setCTC(P->getExpr()->getCTC());
    expr->setType(Q);
    return Q;
}


// return whether Result is valid
bool FunctionAnalyser::analyseBitOffsetIndex(Expr* expr, llvm::APSInt* Result, BuiltinType* BaseType) {
    LOG_FUNC
    QualType T = analyseExpr(expr, RHS);
    if (!T.isValid()) return false;

    if (!T.getCanonicalType().isIntegerType()) {
        StringBuilder buf;
        T.DiagName(buf);
        Diag(expr->getLocation(), diag::err_bitoffset_index_non_int)
            << buf << expr->getSourceRange();
        return false;
    }
    if (!expr->isConstant()) return false;

    LiteralAnalyser LA(Diags);
    llvm::APSInt Val = LA.checkLiterals(expr);
    if (Val.isSigned() && Val.isNegative()) {
        Diag(expr->getLocation(), diag::err_bitoffset_index_negative)
            << Val.toString(10) << expr->getSourceRange();
        return false;
    }
    if (Val.ugt(BaseType->getWidth()-1)) {
        Diag(expr->getLocation(), diag::err_bitoffset_index_too_large)
            << Val.toString(10) << BaseType->getName() << expr->getSourceRange();
        return false;
    }
    *Result = Val;
    return true;
}

QualType FunctionAnalyser::analyseBitOffsetExpr(Expr* expr, QualType BaseType, SourceLocation base) {
    LOG_FUNC
    BitOffsetExpr* B = cast<BitOffsetExpr>(expr);

    // check if base type is unsigned integer
    QualType CT = BaseType.getCanonicalType();
    BuiltinType* BI = dyncast<BuiltinType>(CT.getTypePtr());
    if (!BI || !BI->isInteger() || BI->isSignedInteger()) {
        Diag(base, diag::err_bitoffset_not_unsigned);
        return QualType();
    }

    llvm::APSInt VL;
    bool VLvalid = analyseBitOffsetIndex(B->getLHS(), &VL, BI);
    llvm::APSInt VR;
    bool VRvalid = analyseBitOffsetIndex(B->getRHS(), &VR, BI);

    if (VLvalid && VRvalid) {
        if (VR > VL) {
            Diag(B->getLocation(), diag::err_bitoffset_invalid_order)
                << B->getLHS()->getSourceRange() << B->getRHS()->getSourceRange();
            return QualType();
        } else {
            llvm::APInt width = VL - VR;
            width++;

            QualType T;
            if (width.ult(8+1)) {         T = Type::UInt8();
            } else if (width.ult(16+1)) { T = Type::UInt16();
            } else if (width.ult(32+1)) { T = Type::UInt32();
            } else if (width.ult(64+1)) { T = Type::UInt64();
            } else {
                assert(0 && "should not happen");
            }
            B->setType(T);
            B->setWidth((unsigned char)width.getSExtValue());
        }
    } else {
        B->setType(BaseType);
    }

    if (B->getLHS()->isConstant() && B->getRHS()->isConstant()) B->setConstant();
    combineCtc(B, B->getLHS(), B->getRHS());

    return B->getType();
}

QualType FunctionAnalyser::analyseCall(Expr* expr) {
    LOG_FUNC
    CallExpr* call = cast<CallExpr>(expr);
    // analyse function
    QualType LType = analyseExpr(call->getFn(), RHS);
    if (LType.isNull()) {
        fprintf(stderr, "CALL unknown function (already error)\n");
        call->getFn()->dump();
        return QualType();
    }
    // TODO this should be done in analyseExpr()
    if (!LType.isFunctionType()) {
        char typeName[MAX_LEN_TYPENAME];
        StringBuilder buf(MAX_LEN_TYPENAME, typeName);
        LType.DiagName(buf);
        Diag(call->getLocation(), diag::err_typecheck_call_not_function) << typeName;
        return QualType();
    }

    const FunctionType* FT = cast<FunctionType>(LType);
    FunctionDecl* func = FT->getDecl();
    func->setUsed();
    unsigned protoArgs = func->numArgs();
    unsigned callArgs = call->numArgs();
    unsigned minArgs = MIN(protoArgs, callArgs);
    for (unsigned i=0; i<minArgs; i++) {
        Expr* argGiven = call->getArg(i);
        QualType typeGiven = analyseExpr(argGiven, RHS);
        VarDecl* Arg = func->getArg(i);
        QualType argType = Arg->getType();
        if (typeGiven.isValid()) {
            assert(argType.isValid());
            EA.check(argType, argGiven);
        }
    }
    if (callArgs > protoArgs) {
        // more args given, check if function is variadic
        if (!func->isVariadic()) {
            Expr* arg = call->getArg(minArgs);
            unsigned msg = diag::err_typecheck_call_too_many_args;
            if (func->hasDefaultArgs()) msg = diag::err_typecheck_call_too_many_args_at_most;
            Diag(arg->getLocation(), msg)
                << 0 << protoArgs << callArgs;
            return QualType();
        }
        for (unsigned i=minArgs; i<callArgs; i++) {
            Expr* argGiven = call->getArg(i);
            QualType typeGiven = analyseExpr(argGiven, RHS);
            // TODO use canonical
            if (typeGiven == Type::Void()) {
                fprintf(stderr, "ERROR: (TODO) passing 'void' to parameter of incompatible type '...'\n");
                //Diag(argGiven->getLocation(), diag::err_typecheck_convert_incompatible)
                //    << "from" << "to" << 1;// << argGiven->getLocation();
            }
        }
    } else if (callArgs < protoArgs) {
        // less args given, check for default argument values
        for (unsigned i=minArgs; i<protoArgs; i++) {
            VarDecl* arg = func->getArg(i);
            if (!arg->getInitValue()) {
                if (func->hasDefaultArgs()) {
                    protoArgs = func->minArgs();
                    Diag(arg->getLocation(), diag::err_typecheck_call_too_few_args_at_least)
                        << 0 << protoArgs << callArgs;
                } else {
                    Diag(arg->getLocation(), diag::err_typecheck_call_too_few_args)
                        << 0 << protoArgs << callArgs;
                }
                return QualType();
            }
        }
    }
    call->setType(func->getReturnType());
    return func->getReturnType();
}

Decl* FunctionAnalyser::analyseIdentifier(IdentifierExpr* id) {
    LOG_FUNC
    Decl* D = scope.findSymbol(id->getName(), id->getLocation(), false);
    if (D) {
        id->setDecl(D);
        id->setType(D->getType());
        SetConstantFlags(D, id);
    }
    return D;
}

LabelDecl* FunctionAnalyser::LookupOrCreateLabel(const std::string& name, SourceLocation loc) {
    LOG_FUNC
    LabelDecl* LD = 0;
    for (LabelsIter iter = labels.begin(); iter != labels.end(); ++iter) {
        if ((*iter)->getName() == name) {
            LD = *iter;
            break;
        }
    }
    if (!LD) {
        LD = new LabelDecl(name, loc);
        // add to functionScope
        labels.push_back(LD);
    }
    return LD;
}

bool FunctionAnalyser::checkAssignee(Expr* expr) const {
    switch (expr->getKind()) {
    case EXPR_INTEGER_LITERAL:
    case EXPR_FLOAT_LITERAL:
    case EXPR_BOOL_LITERAL:
    case EXPR_CHAR_LITERAL:
    case EXPR_STRING_LITERAL:
    case EXPR_NIL:
        break;
    case EXPR_CALL:
    case EXPR_IDENTIFIER:
        // ok
        return true;
    case EXPR_INITLIST:
    case EXPR_DESIGNATOR_INIT:
    case EXPR_TYPE:
        break;
    case EXPR_BINOP:
        // ok
        return true;
    case EXPR_CONDOP:
        break;
    case EXPR_UNARYOP:
        // sometimes... (&)
        return true;
    case EXPR_BUILTIN:
        break;
    case EXPR_ARRAYSUBSCRIPT:
    case EXPR_MEMBER:
    case EXPR_PAREN:
    case EXPR_BITOFFSET:
        // ok
        return true;
    }
    // expr is not assignable
    // TODO test (also ternary)
    Diag(expr->getLocation(), diag::err_typecheck_expression_not_modifiable_lvalue);
    return false;
}

void FunctionAnalyser::checkAssignment(Expr* assignee, QualType TLeft) {
    if (TLeft.isConstQualified()) {
        Diag(assignee->getLocation(), diag::err_typecheck_assign_const) << assignee->getSourceRange();
    }
}

void FunctionAnalyser::checkDeclAssignment(Decl* decl, Expr* expr) {
    LOG_FUNC
    assert(decl);
    switch (decl->getKind()) {
    case DECL_FUNC:
        // error
        fprintf(stderr, "TODO cannot assign to non-variable\n");
        break;
    case DECL_VAR:
        {
            //VarDecl* VD = cast<VarDecl>(decl);
            //checkAssignment(expr, VD->getType());
            break;
        }
    case DECL_ENUMVALUE:
    case DECL_ALIASTYPE:
    case DECL_STRUCTTYPE:
    case DECL_ENUMTYPE:
    case DECL_FUNCTIONTYPE:
        Diag(expr->getLocation(), diag::err_typecheck_expression_not_modifiable_lvalue);
        break;
    case DECL_ARRAYVALUE:
        // error
        fprintf(stderr, "TODO cannot assign to non-variable\n");
        break;
    case DECL_IMPORT:
    case DECL_LABEL:
        assert(0);
        break;
    }
}

void FunctionAnalyser::checkArrayDesignators(InitListExpr* expr, int64_t* size) {
    LOG_FUNC
    typedef std::vector<Expr*> Indexes;
    Indexes indexes;
    const ExprList& values = expr->getValues();
    indexes.resize(values.size());
    int maxIndex = 0;
    int currentIndex = -1;
    for (unsigned i=0; i<values.size(); i++) {
        Expr* E = values[i];
        if (DesignatedInitExpr* D = dyncast<DesignatedInitExpr>(E)) {
            currentIndex = D->getIndex().getSExtValue();
            if (currentIndex == -1) return; // some designators are invalid
            if (*size != -1 && currentIndex >= *size) {
                Diag(E->getLocation(), diag::err_array_designator_too_large) << D->getIndex().toString(10) << (int)*size;
                return;
            }
        } else {
            currentIndex++;
        }
        if (*size != -1 && currentIndex >= *size) {
            Diag(E->getLocation(), diag::err_excess_initializers) << 0;
            return;
        }
        if (currentIndex >= indexes.size()) {
            indexes.resize(currentIndex + 1);
        }
        Expr* existing = indexes[currentIndex];
        if (existing) {
            Diag(E->getLocation(), diag::err_duplicate_array_index_init) << E->getSourceRange();
            Diag(existing->getLocation(), diag::note_previous_initializer) << 0 << 0 << E->getSourceRange();
        } else {
            indexes[currentIndex] = E;
        }
        if (currentIndex > maxIndex) maxIndex = currentIndex;
    }
    if (*size == -1) *size = maxIndex + 1;
    //for (unsigned i=0; i<indexes.size(); i++) printf("[%d] = %p\n", i, indexes[i]);
}

void FunctionAnalyser::checkEnumCases(const SwitchStmt* SS, const EnumType* ET) const {
    const EnumTypeDecl* ETD = ET->getDecl();
    char enumHandled[ETD->numConstants()];
    memset(enumHandled, 0, sizeof(enumHandled));
    Stmt* TheDefaultStmt = 0;
    const StmtList& cases = SS->getCases();
    for (unsigned i=0; i<cases.size(); i++) {
        if (isa<DefaultStmt>(cases[i])) {
            TheDefaultStmt = cases[i];
            continue;
        }
        const CaseStmt* CS = cast<CaseStmt>(cases[i]);
        // only allow enum states as cases if switchin on enum (IdentifierExpr -> EnumConstantDecl)
        const Expr* cond = CS->getCond();
        if (const IdentifierExpr* IE = dyncast<IdentifierExpr>(cond)) {
            if (IE->getDecl() == 0) continue;
            // check if it refers to right enumdecl
            if (const EnumConstantDecl* ECD = dyncast<EnumConstantDecl>(IE->getDecl())) {
                int index = ETD->getIndex(ECD);
                if (index != -1) {
                    if (enumHandled[index]) {
                        Diag(cond->getLocation(), diag::err_duplicate_case) << ECD->getName() << cond->getSourceRange();
                    }
                    enumHandled[index] = true;
                    continue;
                }
            }
        }
        StringBuilder buf(64);
        ET->DiagName(buf);
        Diag(cond->getLocation(), diag::warn_not_in_enum) << buf << cond->getSourceRange();
    }

    SmallVector<std::string ,8> UnhandledNames;
    for (unsigned i=0; i<ETD->numConstants(); ++i) {
        if (!enumHandled[i]) UnhandledNames.push_back(ETD->getConstant(i)->getName());
    }

    if (TheDefaultStmt) {
        if (UnhandledNames.empty()) {
            Diag(TheDefaultStmt->getLocation(), diag::warn_unreachable_default);
        }
        return;
    }

    // Produce a nice diagnostic if multiple values aren't handled.
    switch (UnhandledNames.size()) {
    case 0:
        break;
    case 1:
        Diag(SS->getCond()->getLocation(), diag::warn_missing_case1)
            << UnhandledNames[0];
        break;
    case 2:
        Diag(SS->getCond()->getLocation(), diag::warn_missing_case2)
            << UnhandledNames[0] << UnhandledNames[1];
        break;
    case 3:
        Diag(SS->getCond()->getLocation(), diag::warn_missing_case3)
            << UnhandledNames[0] << UnhandledNames[1] << UnhandledNames[2];
        break;
    default:
        Diag(SS->getCond()->getLocation(), diag::warn_missing_cases)
            << (unsigned)UnhandledNames.size()
            << UnhandledNames[0] << UnhandledNames[1] << UnhandledNames[2];
        break;
    }
}

QualType FunctionAnalyser::getStructType(QualType Q) const {
    // Q: use CanonicalType to get rid of AliasTypes?

    Type* T = Q.getTypePtr();
    switch (T->getTypeClass()) {
    case TC_BUILTIN:
        return QualType();
    case TC_POINTER:
        {
            // Could be pointer to structtype
            PointerType* PT = cast<PointerType>(T);
            return getStructType(PT->getPointeeType());
        }
    case TC_ARRAY:
        return QualType();
    case TC_UNRESOLVED:
        assert(0 && "should not happen");
        return QualType();
    case TC_ALIAS:
        {
            AliasType* AT = cast<AliasType>(T);
            return getStructType(AT->getRefType());
        }
    case TC_STRUCT:
        return Q;
    case TC_ENUM:
    case TC_FUNCTION:
    case TC_PACKAGE:
        return QualType();
    }
    assert(0);
}

QualType FunctionAnalyser::getConditionType(const Stmt* C) const {
    if (const Expr* E = dyncast<Expr>(C)) {
        return E->getType();
    }
    if (const DeclStmt* D = dyncast<DeclStmt>(C)) {
        return D->getDecl()->getType();
    }
    assert(0 && "invalid Condition");
    return QualType();
}

DiagnosticBuilder FunctionAnalyser::Diag(SourceLocation Loc, unsigned DiagID) const {
    return Diags.Report(Loc, DiagID);
}

void FunctionAnalyser::pushMode(unsigned DiagID) {
    LOG_FUNC
    inConstExpr = true;
    constDiagID = DiagID;
}

void FunctionAnalyser::popMode() {
    LOG_FUNC
    inConstExpr = false;
    constDiagID = 0;
}

// Convert smaller types to int, others remain the same
// This function should only be called if Expr's type is ok for unary operator
QualType FunctionAnalyser::UsualUnaryConversions(Expr* expr) const {
    const Type* canon = expr->getType().getCanonicalType();
    assert(canon->isBuiltinType());
    const BuiltinType* BI = cast<BuiltinType>(canon);
    if (BI->isPromotableIntegerType()) {
        // TODO keep flags (const, etc)?
        expr->setImpCast(BuiltinType::Int32);
        return Type::Int32();
    }
    return expr->getType();
}


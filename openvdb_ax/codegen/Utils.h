///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2015-2018 DNEG Visual Effects
//
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
//
// Redistributions of source code must retain the above copyright
// and license notice and the following restrictions and disclaimer.
//
// *     Neither the name of DNEG Visual Effects nor the names
// of its contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// IN NO EVENT SHALL THE COPYRIGHT HOLDERS' AND CONTRIBUTORS' AGGREGATE
// LIABILITY FOR ALL CLAIMS REGARDLESS OF THEIR BASIS EXCEED US$250.00.
//
///////////////////////////////////////////////////////////////////////////

/// @file codegen/Utils.h
///
/// @authors Nick Avramoussis
///
/// @brief  Utility code generation methods for performing various llvm
///         operations
///

#ifndef OPENVDB_AX_CODEGEN_UTILS_HAS_BEEN_INCLUDED
#define OPENVDB_AX_CODEGEN_UTILS_HAS_BEEN_INCLUDED

#include "Types.h"

#include <openvdb_ax/ast/Tokens.h>
#include <openvdb_ax/Exceptions.h>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

// Note: As of LLVM 5.0, the llvm::Type::dump() method isn't being
// picked up correctly by the linker. dump() is internally implemented
// using Type::print(llvm::errs()) which is being used in place. See:
//
// https://stackoverflow.com/questions/43723127/llvm-5-0-makefile-undefined-reference-fail
//
#include <llvm/Support/raw_ostream.h> // llvm::errs()

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {

namespace ax {
namespace codegen {

/// @note Function definitions for some types returned from automatic token to
/// llvm IR operations. See llvmArithmeticConversion and llvmBianryConversion

using CastFunction = std::function<llvm::Value*
    (llvm::IRBuilder<>&, llvm::Value*, llvm::Type*)>;

using BinaryFunction = std::function<llvm::Value*
    (llvm::IRBuilder<>&, llvm::Value*, llvm::Value*)>;

/// @brief  Populate a vector of llvm Types from a vector of llvm values
///
/// @param  values  A vector of llvm values to retrieve types from
/// @param  types   A vector of llvm types to populate
///
inline void
valuesToTypes(const std::vector<llvm::Value*>& values,
              std::vector<llvm::Type*>& types)
{
    types.reserve(values.size());
    for (const auto& v : values) {
        types.emplace_back(v->getType());
    }
}

/// @brief  Prints an llvm type to a std string
///
/// @param  type  The llvm type to convert
/// @param  str   The string to store the type info to
///
inline void
llvmTypeToString(const llvm::Type* const type, std::string& str)
{
    llvm::raw_string_ostream os(str);
    type->print(os);
    os.flush();
}

/// @brief  Return the base llvm value which is being pointed to through
///         any number of layered pointers.
/// @note   This function does not check for cyclical pointer dependencies
///
/// @param  type  A llvm pointer type to traverse
///
inline llvm::Type*
getBaseContainedType(llvm::Type* const type)
{
    llvm::Type* elementType = type;
    while (elementType->isPointerTy()) {
        elementType = elementType->getContainedType(0);
    }
    return elementType;
}

/// @brief  Return an llvm value representing a pointer to the provided ptr builtin
///         ValueT.
/// @note   This is probably not a suitable solution for anything other than POD
///         types and should be used with caution.
///
/// @param  ptr      A pointer to a type of ValueT whose address will be computed and
///                  returned
/// @param  builder  The current llvm IRBuilder
///
template <typename ValueT>
inline llvm::Value*
llvmPointerFromAddress(const ValueT* const& ptr,
                       llvm::IRBuilder<>& builder)
{
    llvm::Value* address =
        llvm::ConstantInt::get(llvm::Type::getIntNTy(builder.getContext(), sizeof(uintptr_t)*8),
                               reinterpret_cast<uintptr_t>(ptr));
    return builder.CreateIntToPtr(address, LLVMType<ValueT*>::get(builder.getContext()));
}

/// @brief  Insert a std::string object into IR and return the pointer to it's allocation
/// @note   Includes the null terminator
///
/// @param  string   The string to insert
/// @param  builder  The current llvm IRBuilder
///
inline llvm::Value*
llvmStringToValue(const std::string& string, llvm::IRBuilder<>& builder)
{
    // @todo  replace with llvm::ConstantDataArray::getString

    llvm::LLVMContext& C = builder.getContext();

    const size_t stringSize = string.size();
    llvm::Type* charType = LLVMType<char>::get(C);

    llvm::Value* size = llvm::ConstantInt::get(llvm::Type::getInt64Ty(C), stringSize + 1);
    llvm::Value* store = builder.CreateAlloca(charType, size);

    // loop for <= size to include null terminator

    for (size_t i = 0; i <= string.size(); ++i) {
        llvm::Value* value =
            llvm::ConstantInt::get(charType, uint64_t(string[i]), /*signed*/false);
        llvm::Value* target = builder.CreateConstGEP1_64(store, i);
        builder.CreateStore(value, target);
    }

    return store;
}

/// @brief  Returns the highest order type from two LLVM Scalar types
///
/// @param  typeA  The first scalar llvm type
/// @param  typeB  The second scalar llvm type
///
inline llvm::Type*
typePrecedence(llvm::Type* const typeA,
               llvm::Type* const typeB)
{
    assert(typeA && isScalarType(typeA) &&
        "First Type in typePrecedence is not a scalar type");
    assert(typeB && isScalarType(typeB) &&
        "First Type in typePrecedence is not a scalar type");

    // handle implicit arithmetic conversion
    // (http://osr507doc.sco.com/en/tools/clang_conv_implicit.html)

    if (typeA->isDoubleTy()) return typeA;
    if (typeB->isDoubleTy()) return typeB;

    if (typeA->isFloatTy()) return typeA;
    if (typeB->isFloatTy()) return typeB;

    if (typeA->isIntegerTy(64)) return typeA;
    if (typeB->isIntegerTy(64)) return typeB;

    if (typeA->isIntegerTy(32)) return typeA;
    if (typeB->isIntegerTy(32)) return typeB;

    if (typeA->isIntegerTy(16)) return typeA;
    if (typeB->isIntegerTy(16)) return typeB;

    if (typeA->isIntegerTy(8)) return typeA;
    if (typeB->isIntegerTy(8)) return typeB;

    if (typeA->isIntegerTy(1)) return typeA;
    if (typeB->isIntegerTy(1)) return typeB;

    std::cerr << "Attempted to compare ";
    typeA->print(llvm::errs());
    std::cerr << " to ";
    typeB->print(llvm::errs());
    std::cerr << std::endl;

    OPENVDB_THROW(LLVMTypeError, "Invalid type precedence");
}

/// @brief  Returns a CastFunction which represents the corresponding instruction
///         to convert a source llvm Type to a target llvm Type. If the conversion
///         is unsupported, throws an error.
///
/// @param  sourceType  The source type to cast
/// @param  targetType  The target type to cast to
/// @param  twine       An optional string description of the cast function. This can
///                     be used for for more verbose llvm information on IR compilation
///                     failure
inline CastFunction
llvmArithmeticConversion(const llvm::Type* const sourceType,
                         const llvm::Type* const targetType,
                         const std::string& twine = "")
{

#define BIND_ARITHMETIC_CAST_OP(Function, Twine) \
    std::bind(&Function, \
        std::placeholders::_1, \
        std::placeholders::_2, \
        std::placeholders::_3, \
        Twine)

    if (targetType->isDoubleTy()) {
        if (sourceType->isFloatTy())           return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPExt, twine);
        else if (sourceType->isIntegerTy(64))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSIToFP, twine);
        else if (sourceType->isIntegerTy(32))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSIToFP, twine);
        else if (sourceType->isIntegerTy(16))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSIToFP, twine);
        else if (sourceType->isIntegerTy(8))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSIToFP, twine);
        else if (sourceType->isIntegerTy(1))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateUIToFP, twine);
    }
    else if (targetType->isFloatTy()) {
        if (sourceType->isDoubleTy())          return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPTrunc, twine);
        else if (sourceType->isIntegerTy(64))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSIToFP, twine);
        else if (sourceType->isIntegerTy(32))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSIToFP, twine);
        else if (sourceType->isIntegerTy(16))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSIToFP, twine);
        else if (sourceType->isIntegerTy(8))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSIToFP, twine);
        else if (sourceType->isIntegerTy(1))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateUIToFP, twine);
    }
    else if (targetType->isIntegerTy(64)) {
        if (sourceType->isDoubleTy())          return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPToSI, twine);
        else if (sourceType->isFloatTy())      return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPToSI, twine);
        else if (sourceType->isIntegerTy(32))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSExt, twine);
        else if (sourceType->isIntegerTy(16))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSExt, twine);
        else if (sourceType->isIntegerTy(8))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSExt, twine);
        else if (sourceType->isIntegerTy(1))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateZExt, twine);
    }
    else if (targetType->isIntegerTy(32)) {
        if (sourceType->isDoubleTy())          return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPToSI, twine);
        else if (sourceType->isFloatTy())      return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPToSI, twine);
        else if (sourceType->isIntegerTy(64))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateTrunc, twine);
        else if (sourceType->isIntegerTy(16))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSExt, twine);
        else if (sourceType->isIntegerTy(8))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSExt, twine);
        else if (sourceType->isIntegerTy(1))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateZExt, twine);
    }
    else if (targetType->isIntegerTy(16)) {
        if (sourceType->isDoubleTy())          return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPToSI, twine);
        else if (sourceType->isFloatTy())      return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPToSI, twine);
        else if (sourceType->isIntegerTy(64))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateTrunc, twine);
        else if (sourceType->isIntegerTy(32))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateTrunc, twine);
        else if (sourceType->isIntegerTy(8))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateSExt, twine);
        else if (sourceType->isIntegerTy(1))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateZExt, twine);
    }
    else if (targetType->isIntegerTy(8)) {
        if (sourceType->isDoubleTy())          return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPToSI, twine);
        else if (sourceType->isFloatTy())      return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPToSI, twine);
        else if (sourceType->isIntegerTy(64))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateTrunc, twine);
        else if (sourceType->isIntegerTy(32))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateTrunc, twine);
        else if (sourceType->isIntegerTy(16))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateTrunc, twine);
        else if (sourceType->isIntegerTy(1))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateZExt, twine);
    }
    else if (targetType->isIntegerTy(1)) {
        if (sourceType->isDoubleTy())          return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPToUI, twine);
        else if (sourceType->isFloatTy())      return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateFPToUI, twine);
        else if (sourceType->isIntegerTy(64))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateTrunc, twine);
        else if (sourceType->isIntegerTy(32))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateTrunc, twine);
        else if (sourceType->isIntegerTy(16))  return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateTrunc, twine);
        else if (sourceType->isIntegerTy(8))   return BIND_ARITHMETIC_CAST_OP(llvm::IRBuilder<>::CreateTrunc, twine);
    }

#undef BIND_ARITHMETIC_CAST_OP

    std::cerr << "Attempted to convert ";
    sourceType->print(llvm::errs());
    std::cerr << " to ";
    targetType->print(llvm::errs());
    std::cerr << std::endl;

    OPENVDB_THROW(LLVMTypeError, "Invalid type conversion");
}

/// @brief  Returns a BinaryFunction representing the corresponding instruction to
///         peform on two scalar values, relative to a provided operator token. Note that
///         not all operations are supported on floating point types! If the token is not
///         supported, or the llvm type is not a scalar type, throws an error.
/// @note   Various default arguments are bound to provide a simple function call
///         signature. For floating point operations, this includes a null pointer to
///         the optional metadata node. For integer operations, this includes disabling
///         all overflow/rounding optimisations
///
/// @param  type   The type defining the precision of the binary operation
/// @param  token  The token used to create the relative binary operation
/// @param  twine  An optional string description of the binary function. This can
///                be used for for more verbose llvm information on IR compilation
///                failure
inline BinaryFunction
llvmBinaryConversion(const llvm::Type* const type,
                     const ast::tokens::OperatorToken& token,
                     const std::string& twine = "")
{

#define BIND_FP_BINARY_OP(Function, __Twine) \
    std::bind(&Function, \
        std::placeholders::_1, \
        std::placeholders::_2, \
        std::placeholders::_3, \
        __Twine, \
        nullptr) // MDNode (metadata node - defaults to nullptr)

    // Some integer operations have more than one function with the same name
    // so ensure the correct one is selected
#define BIND_I_BIN_OP(Function, __Twine) \
    std::bind((llvm::Value*(llvm::IRBuilder<>::*)\
        (llvm::Value*, llvm::Value*, const llvm::Twine&))&Function, \
        std::placeholders::_1, \
        std::placeholders::_2, \
        std::placeholders::_3, \
        __Twine)

#define BIND_I_BIN_OP_NW(Function, __Twine) \
    std::bind(&Function, \
        std::placeholders::_1, \
        std::placeholders::_2, \
        std::placeholders::_3, \
        __Twine, \
        /*No Unsigned Wrap*/false, \
        /*No Signed Wrap*/false)

    // NOTE: Binary % and / ops always take sign into account (CreateSDiv vs CreateUDiv, CreateSRem vs CreateURem).
    // See http://stackoverflow.com/questions/5346160/llvm-irbuildercreateudiv-createsdiv-createexactudiv

    if (type->isFloatingPointTy()) {
        const ast::tokens::OperatorType opType = ast::tokens::operatorType(token);
        if (opType == ast::tokens::LOGICAL || opType == ast::tokens::BITWISE) {
            OPENVDB_THROW(LLVMBinaryOperationError, "Unable to perform operation \""
                + ast::tokens::operatorNameFromToken(token) + "\" on floating points values");
        }

        /// @note  Last arguments for FP binary operators
        if (token == ast::tokens::PLUS)                 return BIND_FP_BINARY_OP(llvm::IRBuilder<>::CreateFAdd, twine);
        else if (token == ast::tokens::MINUS)           return BIND_FP_BINARY_OP(llvm::IRBuilder<>::CreateFSub, twine);
        else if (token == ast::tokens::MULTIPLY)        return BIND_FP_BINARY_OP(llvm::IRBuilder<>::CreateFMul, twine);
        else if (token == ast::tokens::DIVIDE)          return BIND_FP_BINARY_OP(llvm::IRBuilder<>::CreateFDiv, twine);
        else if (token == ast::tokens::MODULO)          return BIND_FP_BINARY_OP(llvm::IRBuilder<>::CreateFRem, twine);
        else if (token == ast::tokens::EQUALSEQUALS)    return BIND_FP_BINARY_OP(llvm::IRBuilder<>::CreateFCmpOEQ, twine);
        else if (token == ast::tokens::NOTEQUALS)       return BIND_FP_BINARY_OP(llvm::IRBuilder<>::CreateFCmpONE, twine);
        else if (token == ast::tokens::MORETHAN)        return BIND_FP_BINARY_OP(llvm::IRBuilder<>::CreateFCmpOGT, twine);
        else if (token == ast::tokens::LESSTHAN)        return BIND_FP_BINARY_OP(llvm::IRBuilder<>::CreateFCmpOLT, twine);
        else if (token == ast::tokens::MORETHANOREQUAL) return BIND_FP_BINARY_OP(llvm::IRBuilder<>::CreateFCmpOGE, twine);
        else if (token == ast::tokens::LESSTHANOREQUAL) return BIND_FP_BINARY_OP(llvm::IRBuilder<>::CreateFCmpOLE, twine);
        OPENVDB_THROW(LLVMTokenError, "Unrecognised binary operator \"" +
            ast::tokens::operatorNameFromToken(token) + "\"");
    }
    else if (type->isIntegerTy()) {
        if (token == ast::tokens::PLUS)                  return BIND_I_BIN_OP_NW(llvm::IRBuilder<>::CreateAdd, twine);
        else if (token == ast::tokens::MINUS)            return BIND_I_BIN_OP_NW(llvm::IRBuilder<>::CreateSub, twine);
        else if (token == ast::tokens::MULTIPLY)         return BIND_I_BIN_OP_NW(llvm::IRBuilder<>::CreateMul, twine);
        else if (token == ast::tokens::DIVIDE) {
            return std::bind(&llvm::IRBuilder<>::CreateSDiv,
                std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, twine,
                    false); // IsExact - when true, poison value if the reuslt is rounded
        }
        else if (token == ast::tokens::MODULO)           return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateSRem, twine);
        else if (token == ast::tokens::EQUALSEQUALS)     return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateICmpEQ, twine);
        else if (token == ast::tokens::NOTEQUALS)        return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateICmpNE, twine);
        else if (token == ast::tokens::MORETHAN)         return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateICmpSGT, twine);
        else if (token == ast::tokens::LESSTHAN)         return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateICmpSLT, twine);
        else if (token == ast::tokens::MORETHANOREQUAL)  return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateICmpSGE, twine);
        else if (token == ast::tokens::LESSTHANOREQUAL)  return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateICmpSLE, twine);
        else if (token == ast::tokens::AND)              return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateAnd, twine);
        else if (token == ast::tokens::OR)               return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateOr, twine);
        else if (token == ast::tokens::BITAND)           return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateAnd, twine);
        else if (token == ast::tokens::BITOR)            return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateOr, twine);
        else if (token == ast::tokens::BITXOR)           return BIND_I_BIN_OP(llvm::IRBuilder<>::CreateXor, twine);
        OPENVDB_THROW(LLVMTokenError, "Unrecognised binary operator \"" +
            ast::tokens::operatorNameFromToken(token) + "\"");
    }

#undef BIND_FP_BIN_OP
#undef BIND_I_BIN_OP1
#undef BIND_I_BIN_OP2

    std::cerr << "Attempted to generate a binary operator \""
              << ast::tokens::operatorNameFromToken(token) << "\""
              << "with type ";

    type->print(llvm::errs());

    OPENVDB_THROW(LLVMTypeError, "Invalid type for binary operation");
}

/// @brief  Casts a scalar llvm Value to a target scalar llvm Type. Returns
///         the cast scalar value of type targetType.
///
/// @param value       A llvm scalar value to convert
/// @param targetType  The target llvm scalar type to convert to
/// @param builder     The current llvm IRBuilder
///
inline llvm::Value*
arithmeticConversion(llvm::Value* value,
                     llvm::Type* targetType,
                     llvm::IRBuilder<>& builder)
{
    assert(value && (value->getType()->isIntegerTy() || value->getType()->isFloatingPointTy()) &&
        "First Value in arithmeticConversion is not a scalar type");
    assert(targetType && (targetType->isIntegerTy() || targetType->isFloatingPointTy()) &&
        "Target Type in arithmeticConversion is not a scalar type");

    const llvm::Type* const valueType = value->getType();
    if (valueType == targetType) return value;

    CastFunction llvmCastFunction = llvmArithmeticConversion(valueType, targetType);
    return llvmCastFunction(builder, value, targetType);
}

/// @brief  Casts an array to another array of equal size but of a different element
///         type. Both source and target array element types must be scalar types.
///         The source array llvm Value should be a pointer to the array to cast.
///
/// @param ptrToArray         A llvm value which is a pointer to a llvm array
/// @param targetElementType  The target llvm scalar type to convert each element
///                           of the input array
/// @param builder            The current llvm IRBuilder
///
inline llvm::Value*
arrayCast(llvm::Value* ptrToArray,
          llvm::Type* targetElementType,
          llvm::IRBuilder<>& builder)
{
    assert(targetElementType && (targetElementType->isIntegerTy() ||
        targetElementType->isFloatingPointTy()) &&
        "Target element type is not a scalar type");
    assert(ptrToArray && ptrToArray->getType()->isPointerTy() &&
        "Input to arrayCast is not a pointer type.");

    llvm::Type* arrayType = ptrToArray->getType()->getContainedType(0);
    assert(arrayType && llvm::isa<llvm::ArrayType>(arrayType));

    // getArrayElementType() calls getContainedType(0)
    llvm::Type* sourceElementType = arrayType->getArrayElementType();
    assert(sourceElementType && (sourceElementType->isIntegerTy() ||
        sourceElementType->isFloatingPointTy()) &&
        "Source element type is not a scalar type");

    if (sourceElementType == targetElementType) return ptrToArray;

    CastFunction llvmCastFunction = llvmArithmeticConversion(sourceElementType, targetElementType);

    const size_t elementSize = arrayType->getArrayNumElements();
    llvm::Value* targetArray =
        builder.CreateAlloca(llvm::ArrayType::get(targetElementType, elementSize));

    for (size_t i = 0; i < elementSize; ++i) {
        llvm::Value* target = builder.CreateConstGEP2_64(targetArray, 0, i);
        llvm::Value* source = builder.CreateConstGEP2_64(ptrToArray, 0, i);
        source = builder.CreateLoad(source);
        source = llvmCastFunction(builder, source, targetElementType);
        builder.CreateStore(source, target);
    }

    return targetArray;
}

/// @brief  Converts a vector of loaded llvm scalar values of the same type to a
/// target scalar type. Each value is converted individually and the loaded result
///         stored in the same location within values.
///
/// @param values             A vector of llvm scalar values to convert
/// @param targetElementType  The target llvm scalar type to convert each value
///                           of the input vector
/// @param builder            The current llvm IRBuilder
///
inline void
arithmeticConversion(std::vector<llvm::Value*>& values,
                     llvm::Type* targetElementType,
                     llvm::IRBuilder<>& builder)
{
    assert(targetElementType && (targetElementType->isIntegerTy() ||
        targetElementType->isFloatingPointTy()) &&
        "Target element type is not a scalar type");

    llvm::Type* sourceElementType = values.front()->getType();
    assert(sourceElementType && (sourceElementType->isIntegerTy() ||
        sourceElementType->isFloatingPointTy()) &&
        "Source element type is not a scalar type");

    if (sourceElementType == targetElementType) return;

    CastFunction llvmCastFunction = llvmArithmeticConversion(sourceElementType, targetElementType);

    for (llvm::Value*& value : values) {
        value = llvmCastFunction(builder, value, targetElementType);
    }
}

/// @brief  Converts a vector of loaded llvm scalar values to the highest precision
///         type stored amongst them. Any values which are not scalar types are ignored
///
/// @param values   A vector of llvm scalar values to convert
/// @param builder  The current llvm IRBuilder
///
inline void
arithmeticConversion(std::vector<llvm::Value*>& values,
                     llvm::IRBuilder<>& builder)
{
    llvm::Type* typeCast = LLVMType<bool>::get(builder.getContext());
    for (llvm::Value*& value : values) {
        llvm::Type* type = value->getType();
        if (isScalarType(type)) typeCast = typePrecedence(typeCast, type);
    }

    arithmeticConversion(values, typeCast, builder);
}

/// @brief  Chooses the highest order llvm Type as defined by typePrecedence
///         from either of the two incoming values and casts the other value to
///         the choosen type if it is not already. The types of valueA and valueB
///         are guaranteed to match. Both values must be scalar LLVM types
///
/// @param valueA   The first llvm value
/// @param valueA   The second llvm value
/// @param builder  The current llvm IRBuilder
///
inline void
arithmeticConversion(llvm::Value*& valueA,
                     llvm::Value*& valueB,
                     llvm::IRBuilder<>& builder)
{
    llvm::Type* type = typePrecedence(valueA->getType(), valueB->getType());
    valueA = arithmeticConversion(valueA, type, builder);
    valueB = arithmeticConversion(valueB, type, builder);
}

/// @brief  Performs a C style boolean comparison from a given scalar LLVM value
///
/// @param value    The scalar llvm value to convert to a boolean
/// @param builder  The current llvm IRBuilder
///
inline llvm::Value*
boolComparison(llvm::Value* value,
               llvm::IRBuilder<>& builder)
{
    llvm::Type* type = value->getType();

    if (type->isFloatingPointTy())  return builder.CreateFCmpONE(value, llvm::ConstantFP::get(type, 0.0));
    else if (type->isIntegerTy(1))  return builder.CreateICmpNE(value, llvm::ConstantInt::get(type, 0));
    else if (type->isIntegerTy())   return builder.CreateICmpNE(value, llvm::ConstantInt::getSigned(type, 0));

    std::cerr << "Attempted to convert ";
    type->print(llvm::errs());
    std::cerr << std::endl;

    OPENVDB_THROW(LLVMTypeError, "Invalid type for bool conversion");
}

/// @ brief  Performs a binary operation on two loaded llvm scalar values. The type of
///          operation performed is defined by the token (see the list of supported
///          tokens in ast/Tokens.h. Returns a loaded llvm scalar result
///
/// @param lhs       The left hand side value of the binary operation
/// @param rhs       The right hand side value of the binary operation
/// @param token     The token representing the binary operation to perform
/// @param builder   The current llvm IRBuilder
/// @param warnings  An optional string which will be set to any warnings this function
///                  generates
///
inline llvm::Value*
binaryOperator(llvm::Value* lhs, llvm::Value* rhs,
               const ast::tokens::OperatorToken& token,
               llvm::IRBuilder<>& builder,
               std::string* warnings = nullptr)
{
    llvm::Type* lhsType = lhs->getType();
    if (lhsType != rhs->getType()) {
        std::string error;
        llvm::raw_string_ostream os(error);
        os << "LHS Type: "; lhsType->print(os); os << ", ";
        os << "RHS Type: "; rhs->getType()->print(os); os << " ";
        OPENVDB_THROW(LLVMTypeError, "Mismatching argument types for binary operation \"" +
            ast::tokens::operatorNameFromToken(token) + "\". " + os.str());
    }

    const ast::tokens::OperatorType opType = ast::tokens::operatorType(token);

    if (opType == ast::tokens::LOGICAL) {
        lhs = boolComparison(lhs, builder);
        rhs = boolComparison(rhs, builder);
        lhsType = lhs->getType();
    }
    else if (opType == ast::tokens::BITWISE && lhsType->isFloatingPointTy()) {
        rhs = arithmeticConversion(rhs, LLVMType<int64_t>::get(builder.getContext()), builder);
        lhs = arithmeticConversion(lhs, LLVMType<int64_t>::get(builder.getContext()), builder);
        lhsType = lhs->getType();
        if (warnings) *warnings = std::string("Implicit cast from float to int.");
    }

    const BinaryFunction llvmBinaryFunction = llvmBinaryConversion(lhsType, token);
    return llvmBinaryFunction(builder, lhs, rhs);
}

/// @brief  Unpack a particular element of an array and return a pointer to that element
///         The provided llvm Value is expected to be a pointer to an array
///
/// @param ptrToArray  A llvm value which is a pointer to a llvm array
/// @param index       The index at which to access the array
/// @param builder     The current llvm IRBuilder
///
inline llvm::Value*
arrayIndexUnpack(llvm::Value* ptrToArray,
                 const int16_t index,
                 llvm::IRBuilder<>& builder)
{
    return builder.CreateConstGEP2_64(ptrToArray, 0, index);
}

/// @brief  Unpack an array type into llvm Values which represent all its elements
///         The provided llvm Value is expected to be a pointer to an array
///         If loadElements is true, values will store loaded llvm values instead
///         of pointers to the array elements
///
/// @param ptrToArray    A llvm value which is a pointer to a llvm array
/// @param values        A vector of llvm values where to store the array elements
/// @param builder       The current llvm IRBuilder
/// @param loadElements  Whether or not to load each array element into a register
///
inline void
arrayUnpack(llvm::Value* ptrToArray,
            std::vector<llvm::Value*>& values,
            llvm::IRBuilder<>& builder,
            const bool loadElements = false)
{
    const size_t elements =
        ptrToArray->getType()->getContainedType(0)->getArrayNumElements();

    values.reserve(elements);
    for (size_t i = 0; i < elements; ++i) {
        llvm::Value* value = builder.CreateConstGEP2_64(ptrToArray, 0, i);
        if (loadElements) value = builder.CreateLoad(value);
        values.push_back(value);
    }
}

/// @brief  Unpack the first three elements of an array.
///         The provided llvm Value is expected to be a pointer to an array
/// @note   The elements are note loaded
///
/// @param ptrToArray    A llvm value which is a pointer to a llvm array
/// @param value1        The first array value
/// @param value2        The second array value
/// @param value3        The third array value
/// @param builder       The current llvm IRBuilder
///
inline void
array3Unpack(llvm::Value* ptrToArray,
             llvm::Value*& value1,
             llvm::Value*& value2,
             llvm::Value*& value3,
             llvm::IRBuilder<>& builder)
{
    assert(ptrToArray && ptrToArray->getType()->isPointerTy() &&
        "Input to array3Unpack is not a pointer type.");

    value1 = builder.CreateConstGEP2_64(ptrToArray, 0, 0);
    value2 = builder.CreateConstGEP2_64(ptrToArray, 0, 1);
    value3 = builder.CreateConstGEP2_64(ptrToArray, 0, 2);
}

/// @brief  Pack three values into a new array and return a pointer to the
///         newly allocated array. If the values are of a mismatching type,
///         the highets order type is uses, as defined by typePrecedence. All
///         llvm values are expected to a be a loaded scalar type
///
/// @param value1   The first array value
/// @param value2   The second array value
/// @param value3   The third array value
/// @param builder  The current llvm IRBuilder
///
inline llvm::Value*
array3Pack(llvm::Value* value1,
           llvm::Value* value2,
           llvm::Value* value3,
           llvm::IRBuilder<>& builder)
{
    llvm::Type* type = typePrecedence(value1->getType(), value2->getType());
    type = typePrecedence(type, value3->getType());

    value1 = arithmeticConversion(value1, type, builder);
    value2 = arithmeticConversion(value2, type, builder);
    value3 = arithmeticConversion(value3, type, builder);

    llvm::Type* vectorType = llvm::ArrayType::get(type, 3);
    llvm::Value* vector = builder.CreateAlloca(vectorType);

    llvm::Value* e1 = builder.CreateConstGEP2_64(vector, 0, 0);
    llvm::Value* e2 = builder.CreateConstGEP2_64(vector, 0, 1);
    llvm::Value* e3 = builder.CreateConstGEP2_64(vector, 0, 2);

    builder.CreateStore(value1, e1);
    builder.CreateStore(value2, e2);
    builder.CreateStore(value3, e3);

    return vector;
}

/// @brief  Pack a loaded llvm scalar value into a new array of a specified
///         size and return a pointer to the newly allocated array. Each element
///         of the new array will have the value of the given scalar
///
/// @param value    The uniform scalar llvm value to pack into the array
/// @param builder  The current llvm IRBuilder
/// @param size     The size of the newly allocated array
///
inline llvm::Value*
arrayPack(llvm::Value* value,
          llvm::IRBuilder<>& builder,
          const size_t size = 3)
{
    assert(value && (value->getType()->isIntegerTy() ||
        value->getType()->isFloatingPointTy()) &&
        "value type is not a scalar type");

    llvm::Type* type = value->getType();
    llvm::Value* array =
        builder.CreateAlloca(llvm::ArrayType::get(type, size));

    for (size_t i = 0; i < size; ++i) {
        llvm::Value* element = builder.CreateConstGEP2_64(array, 0, i);
        builder.CreateStore(value, element);
    }

    return array;
}

/// @brief  Pack a vector of loaded llvm scalar values into a new array of
///         equal size and return a pointer to the newly allocated array.
///
/// @param value    A vector of loaded llvm scalar values to pack
/// @param builder  The current llvm IRBuilder
///
inline llvm::Value*
arrayPack(const std::vector<llvm::Value*>& values,
          llvm::IRBuilder<>& builder)
{
    llvm::Type* type = values.front()->getType();
    llvm::Value* array =
        builder.CreateAlloca(llvm::ArrayType::get(type, values.size()));

    size_t idx = 0;
    for (llvm::Value* const& value : values) {
        llvm::Value* element = builder.CreateConstGEP2_64(array, 0, idx++);
        builder.CreateStore(value, element);
    }

    return array;
}

/// @brief  Pack a vector of loaded llvm scalar values into a new array of
///         equal size and return a pointer to the newly allocated array.
///         arrayPackCast first checks all the contained types in values
///         and casts all types to the highest order type present. All llvm
///         values in values are expected to be loaded scalar types
///
/// @param value    A vector of loaded llvm scalar values to pack
/// @param builder  The current llvm IRBuilder
///
inline llvm::Value*
arrayPackCast(std::vector<llvm::Value*>& values,
              llvm::IRBuilder<>& builder)
{
    // get the highest order type present

    llvm::Type* type = LLVMType<bool>::get(builder.getContext());
    for (llvm::Value* const& value : values) {
        type = typePrecedence(type, value->getType());
    }

    // convert all to this type

    for (llvm::Value*& value : values) {
        value = arithmeticConversion(value, type, builder);
    }

    return arrayPack(values, builder);
}

}
}
}
}

#endif // OPENVDB_AX_CODEGEN_UTILS_HAS_BEEN_INCLUDED

// Copyright (c) 2015-2018 DNEG Visual Effects
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )

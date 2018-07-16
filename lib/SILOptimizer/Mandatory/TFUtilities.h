//===--- TFUtilities.h - TensorFlow lowering utilities ----------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This defines the shared code that implements the various TensorFlow related
// lowerings and other transformations.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_TENSORFLOW_H
#define SWIFT_SILOPTIMIZER_TENSORFLOW_H

#include "swift/AST/TensorFlow.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#ifdef SWIFT_ENABLE_TENSORFLOW
#include "tensorflow/c/c_api.h"
#endif

namespace swift {
namespace tf {

/// The device of a tfop instruction (and its output tensors, if any).
enum class DeviceType {
  INVALID,
  CPU, GPU, TPU,
  /// Indicates this instruction should run on all devices in
  /// `GraphGlobalConfiguration::usedDeviceTypes`. For example, a promoted
  /// scalar will run on all such devices, in case it is a loop iteration count
  /// and the loop runs on all devices.
  ALL,
};

/// Must be kepted in sync with the enum class above.
static const int NUM_DEVICE_TYPES = 5;

}  // end namespace tf
}  // end namespace swift

namespace llvm {
template <>
struct DenseMapInfo<::swift::tf::DeviceType> {
  static ::swift::tf::DeviceType getEmptyKey() {
    return ::swift::tf::DeviceType::INVALID;
  }
  static ::swift::tf::DeviceType getTombstoneKey() {
    return ::swift::tf::DeviceType::INVALID;
  }
  static unsigned getHashValue(::swift::tf::DeviceType deviceType) {
    return DenseMapInfo<char>::getHashValue((char)deviceType);
  }
  static bool isEqual(::swift::tf::DeviceType LHS,
                      ::swift::tf::DeviceType RHS) {
    return LHS == RHS;
  }
};
}  // namespace llvm

namespace swift
{
namespace tf {

class DevicePartitionerImpl;

static const char DEFAULT_CPU_DEVICE[] = "/device:CPU:0";
static const char DEFAULT_GPU_DEVICE[] = "/device:GPU:0";
static const char DEFAULT_TPU_DEVICE[] = "TPU_SYSTEM";
// This is a pseudo-device that only exist in the SIL code generated by
// TFPartition and GraphPartitioner, and will be replaced with real devices in
// TFGraphLowering.
static const char ALL_DEVICES[] = "ALL_DEVICES";

// We assume the following special attr names do not occur in the regular
// attributes of any TF ops.
static const char DEVICE_ATTR[] = "__device";
// This pseudo-attribute is propagated from a tfop inst to TensorTransfer, and
// then to D2D send/recv insts. When lowering to TF graph, the pseudo-attribute
// is used when creating TPU infeed/outfeed ops, and is dropped when creating
// other TF ops (e.g. a "Const" op).
static const char SHAPE_ARRAY_ATTR[] = "__shapes";

static DeviceType getOpDeviceType(StringRef device) {
  if (device.str() == DEFAULT_CPU_DEVICE) return DeviceType::CPU;
  if (device.str() == DEFAULT_GPU_DEVICE) return DeviceType::GPU;
  if (device.str() == DEFAULT_TPU_DEVICE) return DeviceType::TPU;
  if (device.str() == ALL_DEVICES) return DeviceType::ALL;

  // FIXME: Consider also supporting variants of the device string, such as
  // "CPU:0".
  llvm_unreachable("Unknown device type");
}

/// The returned string is compatible with TF device name used in TF graphs.
static std::string getDeviceString(DeviceType deviceType) {
  switch (deviceType) {
    case DeviceType::CPU:
      return DEFAULT_CPU_DEVICE;
    case DeviceType::GPU:
      return DEFAULT_GPU_DEVICE;
    case DeviceType::TPU:
      return DEFAULT_TPU_DEVICE;
    case DeviceType::ALL:
      return ALL_DEVICES;
    case DeviceType::INVALID:
      llvm_unreachable("Unsupported device type");
  }
}

/// The returned string can be used to construct SIL function names.
static std::string getDeviceShortName(DeviceType deviceType) {
  switch (deviceType) {
    case DeviceType::CPU:
      return "CPU";
    case DeviceType::GPU:
      return "GPU";
    case DeviceType::TPU:
      return "TPU";
    case DeviceType::ALL:
      return "ALL";
    case DeviceType::INVALID:
      llvm_unreachable("Unsupported device type");
  }
}

/// This struct holds information about the global configuration of the graph
/// we are generating.  This can be different between distinct graphs in the
/// same program though.
//
// TODO: rename this struct.
struct GraphGlobalConfiguration {
  const DeviceType primaryDeviceType;
  const bool isTPUInfeedEnabled;

  // Actual TF devices involved in the tensor computation.
  // It cannot contain DeviceType::ALL.
  llvm::SetVector<DeviceType> usedDeviceTypes;

  /// Return the configuration for the specified function.
  static GraphGlobalConfiguration getForFunction(SILFunction &fn,
                                                 bool removeConfigInst);

  void markDeviceUsed(DeviceType device) {
    if (device != DeviceType::ALL)
      usedDeviceTypes.insert(device);
  }

  // Chooses a device for this tfop, extends `operands` and `newInstName`
  // accordingly with the device attribute, and tracks the chosen device in
  // `usedDeviceTypes`.
  //
  // If `opDevice` is already set, respects that device choice. Otherwise,
  // chooses a device based on this configuration and op kernel device
  // availability.
  //
  // For some tfops (e.g. "tfc.scalarToTensor"), device placement is handled
  // specially, so this function call will be a no-op.
  //
  // TODO: remove this function once we complete the migration to GraphOpInst.
  void handleDevicePlacement(StringRef opType, StringRef opDevice,
                             SILBuilder &B, SILLocation loc,
                             SmallVectorImpl<SILValue> &operands,
                             std::string &newInstName) {
    // No device placement for this special-case "pseudo-op" for
    // scalar-to-tensor promotion. It will later be translated by compiler (in
    // PartitionCloner) into real TF ops, where device placement is handled at
    // that time.
    if (opType == "tfc.scalarToTensor") {
      assert(opDevice.empty());
      return;
    }

    DeviceType chosenDevice;
    if (!opDevice.empty())
      chosenDevice = getOpDeviceType(opDevice);
    else
      chosenDevice = chooseDevice(opType);

    markDeviceUsed(chosenDevice);

    // Example output SIL:
    // %2 = string_literal utf8 "/device:GPU:0"        // user: %3
    // %3 = builtin "__tfop_Const,dtype,value$tensor,__device"(%0 : $@thin
    // %Float.Type, %1 : $Builtin.FPIEEE64, %2 : $Builtin.RawPointer) :
    // %$TensorHandle<Float> // user: %4
    //
    // Note we generate the StringLiteral inst for op device even when the input
    // `opDevice` is not empty. This is redundant but keeps the code simple, and
    // we expect the original StringLiteral inst for the op device to get DCE'd
    // in a later compiler pass.
    auto deviceString = getDeviceString(chosenDevice);
    auto deviceStrInst =
        B.createStringLiteral(loc, StringRef(deviceString),
                              StringLiteralInst::Encoding::UTF8);
    operands.push_back(deviceStrInst);
    newInstName += std::string(",") + DEVICE_ATTR;
  }

  // Choose a device for the graphOpInst under construction, extend `attributes`
  // accordingly with the device attribute, and track the chosen device in
  // `usedDeviceTypes`.
  //
  // If `opDevice` is already set, respects that device choice. Otherwise,
  // chooses a device based on this configuration and op kernel device
  // availability.
  //
  // For some tfops (e.g. "tfc.scalarToTensor"), device placement is handled
  // specially, so this function call will be a no-op.
  //
  void
  handleDevicePlacement(StringRef opType, StringRef opDevice, SILBuilder &B,
                        SILLocation loc,
                        SmallVectorImpl<GraphOperationAttribute> &attributes);

  DeviceType chooseDevice(StringRef opType) const {
    if (opType == "tfc.RecvFromHost" || opType == "tfc.SendToHost")
      return DeviceType::CPU;

    // Place this inst on the device given by this configuration.
    // FIXME: Use the op kernel device availability info to select a device for
    // `opType` -- if that op has no available kernel on `primaryDeviceType`, a
    // different device should be returned.
    return primaryDeviceType;
  }

private:
  GraphGlobalConfiguration(DeviceType primaryDeviceType,
                           bool isTPUInfeedEnabled)
    : primaryDeviceType(primaryDeviceType),
    isTPUInfeedEnabled(isTPUInfeedEnabled) {
    assert(primaryDeviceType != DeviceType::ALL);
    usedDeviceTypes.insert(primaryDeviceType);
  }
};

  /// If the -tf-dump-intermediates flag has been passed, return a pointer to
  /// the stream that we should print debug dump information to.  Otherwise,
  /// return null.  This is used for integration unit tests and debugging.
  llvm::raw_ostream *getTFDumpIntermediateStream();

  /// If the specified type is the well-known TensorHandle<T> type, then return
  /// "T".  If not, return a null type.
  bool isTensorHandle(SILType ty);

  /// Determine whether the specified type is one of our well-known types, and
  /// if so, which one it is.
  TFValueKind classifyTensorFlowValue(SILType ty);

  /// Return true if the specified type is TensorHandle<T>, ResourceHandle, or
  /// VariantHandle.
  bool isTensorFlowValue(SILType ty);

  /// This function maps a Swift type (either a language type like Float or an
  /// LLVM Builtin type like Builtin.f32) into the TensorFlow TF_DataType value.
  unsigned convertSwiftTypeToTF(Type ty);

  /// `ty` must be a valid TensorFlow element type "T", like Builtin.Int32. Turn
  /// it into a TensorHandle<T> type.
  SILType convertElementTypeToTensorValueType(Type ty, const ASTContext& ctx);

  /// If the specified type is a TensorFlow value type, return it.  Otherwise, it
  /// must be a primitive type T.  In that case, wrap it to form TensorHandle<T>.
  SILType convertElementTypeToTensorValueType(SILType ty);

  /// Return true if the specified type is a valid tensor element type.  For
  /// example, int128 and pointers are not.
  ///
  /// TODO: This should eventually consider information about the target
  /// deployment.
  inline bool isValidTensorFlowElementType(Type ty) {
    return convertSwiftTypeToTF(ty) != 0;
  }

  /// Looks up a function in `module`, which must exist.
  /// If needed, load and link it, so that the function body is available to the
  /// caller.
  SILFunction *lookupOrLinkFunction(StringRef name, SILModule &module);

  /// Looks up a function by `name` in the context of `typeDecl`, `proto` and
  /// `module`, and returns that function.
  SILFunction *findSILFunctionForRequiredProtocolMember(
      NominalTypeDecl *typeDecl, ProtocolDecl *proto, DeclName name,
      ModuleDecl *module, SILModule &silModule);

  /// Given an element type like `Float` and a generic signature with a single
  /// type parameter, returns a substitution map suitable for calling a builtin
  /// or function with such a substitution.
  SubstitutionMap getSingleSubstitutionMapForElementTypeAndSignature(
      Type ty, GenericSignature *genericSig);

  /// Given an element type like `Float`, returns a substitution map suitable
  /// for calling a builtin or function with this single-entry substitution.
  SubstitutionMap getSingleSubstitutionMapForElementType(Type ty,
                                                         ASTContext &ctx);

  /// Holds information about a TensorFlow operation as represented in SIL
  /// as Builtin instructions.
  struct SILTensorOpInfo {
    /// The instruction being analyzed.
    BuiltinInst *inst;

    /// This is the name for the entire builtin that we'll partition out.
    StringRef builtinName;

    /// This is the TensorFlow name for the op.
    StringRef opName;

    /// One of these records exists for every operand that the BuiltinInst has,
    /// classifying the operand into a couple of buckets.  The most coarse grain
    /// classification is "input" vs "attribute": the inputs come first,
    /// followed by the attributes.  However, we need to be able to model the
    /// fact that some input arguments are aggregated together into a single
    /// input that is an array of tensors.  An integer attribute may be either
    /// a Tensor value or an integer-encoded DType, etc.
    enum class OperandClass {
      /// This marks three sorts of things:
      /// 1) A normal tensor input: the value is a TensorHandle.
      /// 2) A scalar input suitable for scalar promotion, used by the
      ///    tf.scalarToTensor pseudo-op, the value is a scalar value.
      /// 3) A tensor array (TensorFlow "InputList").  The value is a metatype
      ///    marker value (so we can represent empty arrays) followed by
      ///    InputElt elements that make up the array.
      Input,
      InputElt,     // Element of an input list.  Always a TensorHandle.

      Normal,       // No modifier.
      DType,        // This integer value is a dtype.
      Tensor,       // This array or scalar should be turned into a TF_Tensor.
      Shape,        // This array of integers is a shape specifier.

      Array,        // This marks a normal array value, the value is a metatype.
      ArrayElement, // This is a continuation element of an attribute array.

      // This is the start of a shape array.  The value is the # elements.
      ShapeArray,
    };

    /// Return the string suffix for the specified attribute modifier.
    static const char *getOperandClassSuffix(OperandClass opClass);

    /// Return the operand class of the specified string form like "tensor"
    static llvm::Optional<OperandClass> getOperandClass(StringRef suffix);

    /// These are the names of any attribute operands at the end of the list.
    SmallVector<std::pair<StringRef, OperandClass>, 4> operandClasses;

    /// Return true if the specified operand is an input (not an attribute).
    bool isInput(unsigned operandNumber) const {
      return operandClasses[operandNumber].second == OperandClass::Input ||
             operandClasses[operandNumber].second == OperandClass::InputElt;
    }

    /// Return true if this apply instruction is to a function that can be
    /// conditionally hoisted into the graph, but don't check the operands to
    /// see if they are actually constants we can handle.
    static bool isDecodableApply(ApplyInst *apply);

    /// If the specified call is to a function that we can promote to an op,
    /// rewrite the instruction and return a new one that does so.  Otherwise,
    /// return the same instruction.
    static SILInstruction *decodeApply(ApplyInst *apply);

    /// Analyze the specified SIL instruction and return a SILTensorOpInfo
    /// result if the instruction is a valid tensor operation.  This is the
    /// way that SILTensorOpInfo's are created.
    static Optional<SILTensorOpInfo> decode(SILInstruction *inst);

    /// Verify that all operands to this op are correctly formed, e.g. that
    /// attribute operands are passed acceptable constants.  This returns a
    /// non-empty error string to emit if an error is detected.
    std::string checkAndDiagnoseOperands() const;

    /// Replace any indirect memory operands with direct references to the
    /// scalars they reference.  This potentially replaces the builtin
    /// instruction, so it returns the right one to use.
    ///
    /// This also sets the TF device for the output instruction.
    ///
    /// TODO(clattner): Remove this when deabstraction exists.
    SILInstruction *canonicalizeOperands(
                                     GraphGlobalConfiguration &configuration);

    /// Return the constant instruction that defines the specified attribute
    /// operand, or null if the defining value isn't a valid constant for an
    /// attribute.
    SingleValueInstruction *getAttrOperand(unsigned operandNumber) const {
      return getAttrOperand(inst->getOperand(operandNumber));
    }
    static SingleValueInstruction *getAttrOperand(SILValue v);

    /// Given an array value on which we recently dropped a consuming use, try
    /// to remove all the computation that produces the array if possible.  If
    /// not, emit a destroy_value instruction to avoid leaking it.
    ///
    /// FIXME: Move this logic to deabstraction when it is done.
    ///
    static void removeOrDestroyArrayValue(SILValue array, SILLocation loc,
                                          SILBuilder &B);

    /// Return the device string associated with `inst`, which is required to
    /// exist.
    std::string getDeviceString() const;

    int getIntAttrOperand(unsigned operandNumber, StringRef attrName) const {
      auto operand = inst->getOperand(operandNumber);
      auto opInfo = operandClasses[operandNumber];
      assert(opInfo.first.str() == attrName);
      auto *ili = cast<IntegerLiteralInst>(operand);
      return ili->getValue().getLimitedValue();
    }

    std::string getStringAttrOperand(unsigned operandNumber,
                                     StringRef attrName) const {
      auto operand = inst->getOperand(operandNumber);
      auto opInfo = operandClasses[operandNumber];
      assert(opInfo.first.str() == attrName);
      auto *sli = cast<StringLiteralInst>(operand);
      assert(sli->getEncoding() == StringLiteralInst::Encoding::UTF8);
      return sli->getValue().str();
    }

   private:
    SILTensorOpInfo(BuiltinInst *inst) : inst(inst) {}
    bool decodeBuiltin();
    static SILInstruction *decodeTensorFromScalars(ApplyInst *inst);
    static SILInstruction *decodeTensorFromScalars1D(ApplyInst *inst);
    static SILInstruction *decodeTensorFromScalarsND(ApplyInst *inst);
  };

  /// Holds information about a TensorFlow operation as represented in SIL
  /// as GraphOperationInst.
  struct GraphOperationInfo {
    /// The instruction being analyzed.
    GraphOperationInst *inst;

    explicit GraphOperationInfo(GraphOperationInst *inst) : inst(inst) {}

    /// Return the device attribute associated with `inst`, which is required to
    /// exist.
    StringRef getDeviceString() const;

    /// Return the device type for this instruction.
    DeviceType getDeviceType() const {
      return getOpDeviceType(getDeviceString());
    }

    enum InputMarker {
      /// Scalar input, used by tfc.scalarToTensor only.
      IM_Scalar,
      /// Normal tensor, variant or resource input.
      IM_Normal,
      /// Marker for the start of an input list, has no corresponding operand.
      IM_InputList,
      /// Element of an input list.
      IM_InputListElt,
    };

    /// Return a comma and letter identifier whose letter corresponds to the
    /// specified InputMarker.
    static const char *getInputMarker(InputMarker kind) {
      switch (kind) {
      case IM_Scalar:       return ",s";
      case IM_Normal:       return ",i";
      case IM_InputList:    return ",L";
      case IM_InputListElt: return ",e";
      }
    }

    /// Decode the name of a graph_op into its TensorFlow op name and a list of
    /// information about the operands.
    StringRef decodeName(SmallVectorImpl<InputMarker> &inputInfo);

    /// Given an attribute name like foo$dtype, decode the name and the class.
    /// If there is no modifier specified, this defaults to
    /// OperandClass::Normal.
    static std::pair<StringRef, SILTensorOpInfo::OperandClass>
    decodeAttributeName(Identifier name);

    /// Given a SILValue that may be an array literal, attempt to decode it into
    /// the values that make up its elements.  If this fails or if the value is
    /// not an array, this returns a null Type.  Otherwise it decodes the array,
    /// returns the values of each element, and returns the element type of the
    /// array.
    ///
    /// If arrayInsts is non-null and if decoding succeeds, this function adds
    /// all of the instructions relevant to the definition of this array into
    /// the set.  If decoding fails, then the contents of this set is undefined.
    static Type decodeArrayElements(SILValue value,
                                    SmallVectorImpl<SILValue> &elements,
                        SmallPtrSet<SILInstruction*, 8> *arrayInsts = nullptr);

  private:
    void assertWithDump(bool cond, const char *assertMsg) const;
  };

  //===--------------------------------------------------------------------===//
  // Source location helpers
  //===--------------------------------------------------------------------===//

  /// The SIL location for operations we process are usually deep in the bowels
  /// of the tensor library code, which are all implementation details to the
  /// user.  As such, walk the inlining location of the specified node to return
  /// the first location *outside* of the tensor implementation goop.
  SILDebugLocation skipInternalLocations(SILDebugLocation loc);

  /// Skip over all the internal implementation details to get the source
  ///  location in user code.
  inline SILLocation getUserSourceLocation(SILDebugLocation loc) {
    return skipInternalLocations(loc).getLocation();
  }

  /// Get the user's source location for the specified value.  If it is an
  /// instruction, we can apply various heuristics to improve the precision of
  /// the returned location information.
  SILLocation getUserSourceLocation(SILValue value);
  SILLocation getUserSourceLocation(SILInstruction *inst);

  //===--------------------------------------------------------------------===//
  // Other stuff
  //===--------------------------------------------------------------------===//

  /// This struct provides a an efficient implementation of a predicate that
  /// determines whether a type is or contains a TensorHandle that will be
  /// exposed after deabstraction.  This is a class instead of a simple function
  /// because we memoize state to avoid rechecking types over and over again.
  class TensorFunctionClassifier {
    TypeContainsTensorFlowValue tctfc;
  public:
    TensorFunctionClassifier() {}

    /// Return true if the specified function is the top-level context that
    /// tensor partitioning should be applied to.  This returns false (for
    /// example) for inlined functions that take and return tensors, since we
    /// know that they are either unreachable or will be inlined into any
    /// clients that use them.
    bool shouldBePartitioned(SILFunction *fn);

    /// Return true if the specified function type has TensorFlow values in its
    /// argument or result list, even if they are abstracted by structs or
    /// tuples.
    bool containsTensorFlowValue(CanSILFunctionType fnType);

    /// Return true if the specified type contains a TensorFlow value type that
    /// will be exposed after deabstraction.
    bool containsTensorFlowValue(Type ty) {
      return tctfc.containsTensorFlowValue(ty);
    }

    /// Return true if the specified type contains a TensorFlow value type that
    /// will be exposed after deabstraction.
    bool containsTensorFlowValue(SILType ty) {
      return containsTensorFlowValue(ty.getASTType());
    }

  };

  /// Partitions an accelerator SIL function into a set of per-device SIL
  /// functions.
  class DevicePartitioner {
    DevicePartitionerImpl* impl;

   public:
    DevicePartitioner(SILFunction &srcFn,
                     const GraphGlobalConfiguration &configuration);

    ~DevicePartitioner();

    /// Returns a function extracted from `srcFn`, specialized on `deviceType`.
    ///
    /// For example, say `fn` returns a+b, where a and b and constant tensors,
    /// and a is placed on GPU.
    /// - The extracted function for GPU device has the constant node a, fed
    /// into
    ///   a _Send() node to CPU.
    /// - The extracted function for CPU device has _Recv node from GPU to read
    ///   a, and adds its output with const tensor b to produce the sum result.
    SILFunction *extractFunctionForDevice(DeviceType deviceType);
  };

  /// Represent the TF graph of a graph function named `graphFnName`, which
  /// corresponds to the SIL host function `silHostFnName`. `graph` can contain
  /// more functions beyond `graphFnName`, if that function calls into other
  /// graph functions (e.g. if it has functional If/While ops).
  struct LoweredGraphFunction {
    LoweredGraphFunction(
        const std::string &silHostFnName, const std::string &graphFnName,
        std::unique_ptr<TF_Graph, decltype(&TF_DeleteGraph)> graph,
        SmallVector<std::pair<StringRef, SILLocation>, 1> pendingGraphFnNames)
        : silHostFnName(silHostFnName), graphFnName(graphFnName),
          graph(std::move(graph)),
          pendingGraphFnNames(std::move(pendingGraphFnNames)) {}

    LoweredGraphFunction(LoweredGraphFunction &&) = delete;

    /// Used as the buffer to back a StringRef-typed map key value elsewhere.
    std::string silHostFnName;

    std::string graphFnName;

    std::unique_ptr<TF_Graph, decltype(&TF_DeleteGraph)> graph;

    /// Each entry track a "pending" graph function F (via its host function
    /// name) referenced by this function, along with the source location
    /// indicating where this function references F.
    /// "Pending" means the graph definition of F is not yet available. Once it
    /// is generated later, it will be copied over to `graph` so that `graph`
    /// becomes self-contained.
    SmallVector<std::pair<StringRef, SILLocation>, 1> pendingGraphFnNames;
  };

  /// Lower the accelerator-only function `fn` (which was formed by the
  /// partitioner) into a TensorFlow graph function, and add an entry to
  /// `graphFunctions`, keyed on `hostFnName`. This way another graph function
  /// foo() can call/use this function, if the corresponding SIL code of foo()
  /// calls/uses `hostFnName`.
  bool lowerTFFunction(
      StringRef hostFnName, SILFunction *fn,
      const GraphGlobalConfiguration &configuration,
      llvm::DenseMap<StringRef, std::unique_ptr<LoweredGraphFunction>>
          &graphFunctions);

  /// Similar to the function above, except it handles a non-accelerator-only
  /// function, which can be lowered to graph functions on a set of TF devices.
  ///
  /// When configuration.usedDeviceTypes has N>1 devices, in addition to
  /// generating a graph function whose name is
  /// LoweredGraphFunction::graphFnName (referred to as `entryFnBaseName`), also
  /// generate another N-1 nodes named `entryFnBaseName_helper_{i}`, with i
  /// ranging from 0 to N-2. These N nodes correspond to the N per-device graph
  /// functions, and must be called by the runtime in a single SessionRun()
  /// call. Those N-1 helper functions take no input or output tensors, and are
  /// executed for their side-effects of sending/receiving tensors with the
  /// function of `entryFnBaseName`.
  bool
  lowerTFGraph(StringRef hostFnName, SILFunction *fn,
               const GraphGlobalConfiguration &configuration,
               llvm::DenseMap<StringRef, std::unique_ptr<LoweredGraphFunction>>
                   &graphFunctions);

  /// Copy the graphs functions in `srcGraph` to `resultGraph`, and verify that
  /// `graphFuncName` must be one of the graph functions to copy over. Return
  /// true on error, with error already emitted on `fn` and `loc`.
  bool copyGraphFunctions(SILFunction &fn, SILLocation loc,
                          StringRef graphFuncName, TF_Graph *srcGraph,
                          TF_Graph *resultGraph);

  /// Serialize `resultGraph` into a binary protobuf into `bytes`.
  /// Return true on error, with a error diagnostic already emitted.
  bool serializeGraphProtoBuf(SILFunction &SILFn, TF_Graph *resultGraph,
                              std::vector<char> &bytes);

} // end namespace tf
} // end namespace swift
#endif

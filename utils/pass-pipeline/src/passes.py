
from pass_pipeline import Pass

# TODO: This should not be hard coded. Create a tool in the compiler that knows
# how to dump the passes and the pipelines themselves.
AADumper = Pass('AADumper')
ABCOpt = Pass('ABCOpt')
AllocBoxToStack = Pass('AllocBoxToStack')
CFGPrinter = Pass('CFGPrinter')
COWArrayOpts = Pass('COWArrayOpts')
CSE = Pass('CSE')
CapturePromotion = Pass('CapturePromotion')
CapturePropagation = Pass('CapturePropagation')
ClosureSpecializer = Pass('ClosureSpecializer')
CodeMotion = Pass('CodeMotion')
CopyForwarding = Pass('CopyForwarding')
DCE = Pass('DCE')
DeadFunctionElimination = Pass('DeadFunctionElimination')
DeadObjectElimination = Pass('DeadObjectElimination')
DefiniteInitialization = Pass('DefiniteInitialization')
Devirtualizer = Pass('Devirtualizer')
DiagnoseUnreachable = Pass('DiagnoseUnreachable')
DiagnosticConstantPropagation = Pass('DiagnosticConstantPropagation')
EarlyInliner = Pass('EarlyInliner')
EmitDFDiagnostics = Pass('EmitDFDiagnostics')
FunctionSignatureOpts = Pass('FunctionSignatureOpts')
GenericSpecializer = Pass('GenericSpecializer')
GlobalARCOpts = Pass('GlobalARCOpts')
GlobalLoadStoreOpts = Pass('GlobalLoadStoreOpts')
GlobalOpt = Pass('GlobalOpt')
IVInfoPrinter = Pass('IVInfoPrinter')
InOutDeshadowing = Pass('InOutDeshadowing')
InlineCaches = Pass('InlineCaches')
InstCount = Pass('InstCount')
LICM = Pass('LICM')
LateInliner = Pass('LateInliner')
LoopInfoPrinter = Pass('LoopInfoPrinter')
LoopRotate = Pass('LoopRotate')
LowerAggregateInstrs = Pass('LowerAggregateInstrs')
MandatoryInlining = Pass('MandatoryInlining')
Mem2Reg = Pass('Mem2Reg')
NoReturnFolding = Pass('NoReturnFolding')
PerfInliner = Pass('PerfInliner')
PerformanceConstantPropagation = Pass('PerformanceConstantPropagation')
PredictableMemoryOptimizations = Pass('PredictableMemoryOptimizations')
SILCleanup = Pass('SILCleanup')
SILCombine = Pass('SILCombine')
SILLinker = Pass('SILLinker')
SROA = Pass('SROA')
SimplifyCFG = Pass('SimplifyCFG')
SplitAllCriticalEdges = Pass('SplitAllCriticalEdges')
SplitNonCondBrCriticalEdges = Pass('SplitNonCondBrCriticalEdges')
StripDebugInfo = Pass('StripDebugInfo')
SwiftArrayOpts = Pass('SwiftArrayOpts')

PASSES = [
    AADumper,
    ABCOpt,
    AllocBoxToStack,
    CFGPrinter,
    COWArrayOpts,
    CSE,
    CapturePromotion,
    CapturePropagation,
    ClosureSpecializer,
    CodeMotion,
    CopyForwarding,
    DCE,
    DeadFunctionElimination,
    DeadObjectElimination,
    DefiniteInitialization,
    Devirtualizer,
    DiagnoseUnreachable,
    DiagnosticConstantPropagation,
    EarlyInliner,
    EmitDFDiagnostics,
    FunctionSignatureOpts,
    GenericSpecializer,
    GlobalARCOpts,
    GlobalLoadStoreOpts,
    GlobalOpt,
    IVInfoPrinter,
    InOutDeshadowing,
    InlineCaches,
    InstCount,
    LICM,
    LateInliner,
    LoopInfoPrinter,
    LoopRotate,
    LowerAggregateInstrs,
    MandatoryInlining,
    Mem2Reg,
    NoReturnFolding,
    PerfInliner,
    PerformanceConstantPropagation,
    PredictableMemoryOptimizations,
    SILCleanup,
    SILCombine,
    SILLinker,
    SROA,
    SimplifyCFG,
    SplitAllCriticalEdges,
    SplitNonCondBrCriticalEdges,
    StripDebugInfo,
    SwiftArrayOpts,
]

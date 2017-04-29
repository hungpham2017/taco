#include "taco/tensor_base.h"

#include <sstream>

#include "taco/tensor.h"
#include "taco/expr.h"
#include "taco/format.h"
#include "ir/ir.h"
#include "taco/storage/storage.h"
#include "taco/storage/pack.h"
#include "lower/lower.h"
#include "lower/iteration_schedule.h"
#include "backends/module.h"
#include "taco/io/tns_file_format.h"
#include "taco/io/mtx_file_format.h"
#include "taco/io/rb_file_format.h"
#include "taco/util/strings.h"
#include "taco/util/timers.h"

using namespace std;
using namespace taco::ir;
using namespace taco::storage;

namespace taco {

struct TensorBase::Content {
  string                   name;
  vector<int>              dimensions;
  ComponentType            ctype;

  storage::Storage         storage;

  vector<taco::Var>        indexVars;
  taco::Expr               expr;
  vector<void*>            arguments;

  size_t                   allocSize;
  size_t                   valuesSize;

  lower::IterationSchedule schedule;
  Stmt                     assembleFunc;
  Stmt                     computeFunc;
  shared_ptr<Module>       module;
};

TensorBase::TensorBase() : TensorBase(ComponentType::Double) {
}

TensorBase::TensorBase(ComponentType ctype)
    : TensorBase(util::uniqueName('A'), ctype) {
}

TensorBase::TensorBase(std::string name, ComponentType ctype)
    : TensorBase(name, ctype, {}, Format(), 1)  {
}

TensorBase::TensorBase(string name, ComponentType ctype, vector<int> dimensions)
  : TensorBase(name, ctype, dimensions,
               Format(vector<LevelType>(dimensions.size(),LevelType::Sparse))) {
}

TensorBase::TensorBase(ComponentType ctype, vector<int> dimensions)
    : TensorBase(util::uniqueName('A'), ctype, dimensions) {
}

TensorBase::TensorBase(ComponentType ctype, vector<int> dimensions,
                       Format format, size_t allocSize)
    : TensorBase(util::uniqueName('A'), ctype, dimensions, format, allocSize) {
}

TensorBase::TensorBase(string name, ComponentType ctype, vector<int> dimensions,
                       Format format, size_t allocSize) : content(new Content) {
  taco_uassert(format.getLevels().size() == dimensions.size())
      << "The number of format levels (" << format.getLevels().size()
      << ") must match the tensor order (" << dimensions.size() << ")";
  taco_uassert(ctype == ComponentType::Double)
      << "Only double tensors currently supported";

  content->name = name;
  content->dimensions = dimensions;
  content->storage = Storage(format);
  content->ctype = ctype;
  content->allocSize = allocSize;

  // Initialize dense storage dimensions
  vector<Level> levels = format.getLevels();
  for (size_t i=0; i < levels.size(); ++i) {
    if (levels[i].getType() == LevelType::Dense) {
      Storage::LevelIndex& index = content->storage.getLevelIndex(i);
      index.ptr = (int*)malloc(sizeof(int));
      index.ptr[0] = dimensions[i];
    }
  }
  
  content->module = make_shared<Module>();

  this->coordinateBuffer = shared_ptr<vector<char>>(new vector<char>);
  this->coordinateBufferUsed = 0;
  this->coordinateSize = getOrder()*sizeof(int) + ctype.bytes();
}

string TensorBase::getName() const {
  return content->name;
}

size_t TensorBase::getOrder() const {
  return content->dimensions.size();
}

const vector<int>& TensorBase::getDimensions() const {
  return content->dimensions;
}

const Format& TensorBase::getFormat() const {
  return content->storage.getFormat();
}

const ComponentType& TensorBase::getComponentType() const {
  return content->ctype;
}

const vector<taco::Var>& TensorBase::getIndexVars() const {
  return content->indexVars;
}

const taco::Expr& TensorBase::getExpr() const {
  return content->expr;
}

const storage::Storage& TensorBase::getStorage() const {
  return content->storage;
}

storage::Storage TensorBase::getStorage() {
  return content->storage;
}

size_t TensorBase::getAllocSize() const {
  return content->allocSize;
}

void TensorBase::setFormat(Format format) {
  content->storage.setFormat(format);
}

void TensorBase::setCSR(double* vals, int* rowPtr, int* colIdx) {
  taco_uassert(getFormat() == CSR) <<
      "setCSR: the tensor " << getName() << " is not defined in the CSR format";
  auto S = getStorage();
  std::vector<int> denseDim = {getDimensions()[0]};
  Storage::LevelIndex d0Index(util::copyToArray(denseDim), nullptr);
  Storage::LevelIndex d1Index(rowPtr, colIdx);
  S.setLevelIndex(0, d0Index);
  S.setLevelIndex(1, d1Index);
  S.setValues(vals);
}

void TensorBase::getCSR(double** vals, int** rowPtr, int** colIdx) {
  taco_uassert(getFormat() == CSR) <<
      "getCSR: the tensor " << getName() << " is not defined in the CSR format";
  auto S = getStorage();
  *vals = S.getValues();
  *rowPtr = S.getLevelIndex(1).ptr;
  *colIdx = S.getLevelIndex(1).idx;
}

void TensorBase::setCSC(double* vals, int* colPtr, int* rowIdx) {
  taco_uassert(getFormat() == CSC) <<
      "setCSC: the tensor " << getName() << " is not defined in the CSC format";
  auto S = getStorage();
  std::vector<int> denseDim = {getDimensions()[1]};
  Storage::LevelIndex d0Index(util::copyToArray(denseDim), nullptr);
  Storage::LevelIndex d1Index(colPtr, rowIdx);
  S.setLevelIndex(0, d0Index);
  S.setLevelIndex(1, d1Index);
  S.setValues(vals);
}

void TensorBase::getCSC(double** vals, int** colPtr, int** rowIdx) {
  taco_uassert(getFormat() == CSC) <<
      "getCSC: the tensor " << getName() << " is not defined in the CSC format";

  auto S = getStorage();
  *vals = S.getValues();
  *colPtr = S.getLevelIndex(1).ptr;
  *rowIdx = S.getLevelIndex(1).idx;
}

void TensorBase::read(std::string filename) {
  std::string extension = filename.substr(filename.find_last_of(".") + 1);
  if(extension == "rb") {
    readHB(filename);
  }
  else {
    taco_uerror << "file extension not supported " << filename << std::endl;
  }
}

void TensorBase::readHB(std::string filename) {
  taco_uassert(getFormat() == CSC) <<
      "readHB: the tensor " << getName() << " is not defined in the CSC format";
  std::ifstream hbfile;

  hbfile.open(filename.c_str());
  taco_uassert(hbfile.is_open()) <<
      " Error opening the file " << filename.c_str();
  int nrow, ncol;
  int *colptr = NULL;
  int *rowind = NULL;
  double *values = NULL;

  rb::readFile(hbfile, &nrow, &ncol, &colptr, &rowind, &values);
  taco_uassert((nrow==getDimensions()[0]) && (ncol==getDimensions()[1])) <<
      "readHB: the tensor " << getName() <<
      " does not have the same dimension in its declaration and HBFile" <<
  filename.c_str();

  auto storage = getStorage();
  std::vector<int> denseDim = {getDimensions()[1]};
  Storage::LevelIndex d0Index(util::copyToArray(denseDim), nullptr);
  Storage::LevelIndex d1Index(colptr, rowind);
  storage.setLevelIndex(0, d0Index);
  storage.setLevelIndex(1, d1Index);
  storage.setValues(values);

  hbfile.close();
}

void TensorBase::writeHB(std::string filename) const {
  taco_uassert(getFormat() == CSC) <<
      "writeHB: the tensor " << getName() <<
      " is not defined in the CSC format";
  std::ofstream HBfile;

  HBfile.open(filename.c_str());
  taco_uassert(HBfile.is_open()) <<
      " Error opening the file " << filename.c_str();

  auto S = getStorage();
  auto size = S.getSize();

  double *values = S.getValues();
  int *colptr = S.getLevelIndex(1).ptr;
  int *rowind = S.getLevelIndex(1).idx;
  int nrow = getDimensions()[0];
  int ncol = getDimensions()[1];
  int nnzero = size.values;
  std::string key = getName();
  int valsize = size.values;
  int ptrsize = size.indexSizes[1].ptr;
  int indsize = size.indexSizes[1].idx;

  rb::writeFile(HBfile,const_cast<char*> (key.c_str()),
                nrow,ncol,nnzero,
                ptrsize,indsize,valsize,
                colptr,rowind,values);

  HBfile.close();
}

static int numIntegersToCompare = 0;
static int lexicographicalCmp(const void* a, const void* b) {
  for (int i = 0; i < numIntegersToCompare; i++) {
    int diff = ((int*)a)[i] - ((int*)b)[i];
    if (diff != 0) {
      return diff;
    }
  }
  return 0;
}

/// Pack coordinates into a data structure given by the tensor format.
void TensorBase::pack() {
  taco_tassert(getComponentType() == ComponentType::Double)
      << "make the packing machinery work with other primitive types later. "
      << "Right now we're specializing to doubles so that we can use a "
      << "resizable vector, but eventually we should use a two pass pack "
      << "algorithm that figures out sizes first, and then packs the data";

  // Nothing to pack
  if (coordinateBufferUsed == 0) {
    return;
  }
  const size_t order = getOrder();


  // Pack scalars
  if (order == 0) {
    content->storage.setValues((double*)malloc(getComponentType().bytes()));
    char* coordLoc = this->coordinateBuffer->data();
    content->storage.getValues()[0] =
        *(double*)&coordLoc[this->coordinateSize-getComponentType().bytes()];
    this->coordinateBuffer->clear();
    return;
  }


  /// Permute the coordinates according to the storage dimension ordering.
  /// This is a workaround since the current pack code only packs tensors in the
  /// order of the dimensions.
  const std::vector<Level>& levels     = getFormat().getLevels();
  const std::vector<int>&   dimensions = getDimensions();
  taco_iassert(levels.size() == order);
  std::vector<int> permutation;
  for (auto& level : levels) {
    permutation.push_back((int)level.getDimension());
  }

  std::vector<int> permutedDimensions(order);
  for (size_t i = 0; i < order; ++i) {
    permutedDimensions[i] = dimensions[permutation[i]];
  }

  taco_iassert((this->coordinateBufferUsed % this->coordinateSize) == 0);
  size_t numCoordinates = this->coordinateBufferUsed / this->coordinateSize;
  const size_t coordSize = this->coordinateSize;

  char* coordinatesPtr = coordinateBuffer->data();
  vector<int> permuteBuffer(order);
  for (size_t i=0; i < numCoordinates; ++i) {
    int* coordinate = (int*)coordinatesPtr;
    for (size_t j = 0; j < order; j++) {
      permuteBuffer[j] = coordinate[permutation[j]];
    }
    for (size_t j = 0; j < order; j++) {
      coordinate[j] = permuteBuffer[j];
    }
    coordinatesPtr += this->coordinateSize;
  }
  coordinatesPtr = coordinateBuffer->data();


  // The pack code expects the coordinates to be sorted
  numIntegersToCompare = order;
  qsort(coordinatesPtr, numCoordinates, coordSize, lexicographicalCmp);


  // Move coords into separate arrays
  std::vector<std::vector<int>> coordinates(order);
  for (size_t i=0; i < order; ++i) {
    coordinates[i] = std::vector<int>(numCoordinates);
  }

  std::vector<double> values(numCoordinates);
  for (size_t i=0; i < numCoordinates; ++i) {
    int* coordLoc = (int*)&coordinatesPtr[i*coordSize];
    for (size_t d=0; d < order; ++d) {
      coordinates[d][i] = *coordLoc;
      coordLoc++;
    }
    values[i] = *((double*)coordLoc);
  }
  taco_iassert(coordinates.size() > 0);
  this->coordinateBuffer->clear();
  this->coordinateBufferUsed = 0;


  // Pack indices and values
  content->storage = storage::pack(permutedDimensions, getFormat(),
                                   coordinates, values);

//  ir::Stmt packFunc = storage::packCode(getFormat());
//  std::cout << getFormat() << std::endl;
//  std::cout << packFunc << std::endl;
}

void TensorBase::zero() {
  auto resultStorage = getStorage();
  // Set values to 0.0 in case we are doing a += operation
  memset(resultStorage.getValues(), 0, content->valuesSize * sizeof(double));
}

void TensorBase::compile() {
  taco_iassert(getExpr().defined()) << "No expression defined for tensor";
  content->assembleFunc = lower::lower(*this, "assemble", {lower::Assemble});
  content->computeFunc  = lower::lower(*this, "compute", {lower::Compute});
  content->module->addFunction(content->assembleFunc);
  content->module->addFunction(content->computeFunc);
  content->module->compile();
}

static inline vector<void*> packArguments(const TensorBase& tensor) {
  vector<void*> arguments;

  // Pack the result tensor
  auto resultStorage = tensor.getStorage();
  auto resultFormat = resultStorage.getFormat();
  for (size_t i=0; i<resultFormat.getLevels().size(); i++) {
    Storage::LevelIndex levelIndex = resultStorage.getLevelIndex(i);
    auto& levelFormat = resultFormat.getLevels()[i];
    switch (levelFormat.getType()) {
      case Dense:
        arguments.push_back((void*)levelIndex.ptr);
        break;
      case Sparse:
      case Fixed:
        arguments.push_back((void*)levelIndex.ptr);
        arguments.push_back((void*)levelIndex.idx);
        break;
    }
  }
  arguments.push_back((void*)resultStorage.getValues());

  // Pack operand tensors
  vector<TensorBase> operands = expr_nodes::getOperands(tensor.getExpr());
  for (auto& operand : operands) {
    Storage storage = operand.getStorage();
    Format format = storage.getFormat();
    for (size_t i=0; i<format.getLevels().size(); i++) {
      Storage::LevelIndex levelIndex = storage.getLevelIndex(i);
      auto& levelFormat = format.getLevels()[i];
      switch (levelFormat.getType()) {
        case Dense:
          arguments.push_back((void*)levelIndex.ptr);
          break;
        case Sparse:
        case Fixed:
          arguments.push_back((void*)levelIndex.ptr);
          arguments.push_back((void*)levelIndex.idx);
          break;
      }
    }
    arguments.push_back((void*)storage.getValues());
  }

  return arguments;
}

void TensorBase::assemble() {
  this->content->arguments = packArguments(*this);
  this->assembleInternal();
}

void TensorBase::compute() {
  this->content->arguments = packArguments(*this);
  this->zero();
  this->computeInternal();
}

void TensorBase::evaluate() {
  this->compile();
  this->content->arguments = packArguments(*this);
  this->assembleInternal();
  this->zero();
  this->computeInternal();
}

void TensorBase::setExpr(taco::Expr expr) {
  content->expr = expr;

  storage::Storage storage = getStorage();
  Format format = storage.getFormat();
  auto& levels = format.getLevels();
  for (size_t i=0; i < levels.size(); ++i) {
    Level level = levels[i];
    auto& levelIndex = storage.getLevelIndex(i);
    switch (level.getType()) {
      case LevelType::Dense:
        break;
      case LevelType::Sparse:
        levelIndex.ptr = (int*)malloc(getAllocSize() * sizeof(int));
        levelIndex.ptr[0] = 0;
        levelIndex.idx = (int*)malloc(getAllocSize() * sizeof(int));
        break;
      case LevelType::Fixed:
        levelIndex.ptr = (int*)malloc(sizeof(int));
        levelIndex.idx = (int*)malloc(getAllocSize() * sizeof(int));
        break;
    }
  }
}

void TensorBase::setIndexVars(vector<taco::Var> indexVars) {
  content->indexVars = indexVars;
}

void TensorBase::printComputeIR(std::ostream& os, bool color) const {
  IRPrinter printer(os,color);
  content->computeFunc.as<Function>()->body.accept(&printer);
}

void TensorBase::printAssemblyIR(std::ostream& os, bool color) const {
  IRPrinter printer(os,color);
  content->assembleFunc.as<Function>()->body.accept(&printer);
}

string TensorBase::getSource() const {
  return content->module->getSource();
}

void TensorBase::compileSource(std::string source) {
  taco_iassert(getExpr().defined()) << "No expression defined for tensor";
  content->module->setSource(source);
  content->module->compile();
}

bool equals(const TensorBase& a, const TensorBase& b) {
  // Component type must be the same
  if (a.getComponentType() != b.getComponentType()) {
    return false;
  }

  // Dimensions must be the same
  if (a.getOrder() != b.getOrder()) {
    return false;
  }
  for (auto dims : util::zip(a.getDimensions(), b.getDimensions())) {
    if (dims.first != dims.second) {
      return false;
    }
  }

  // Values must be the same
  auto ait = a.begin();
  auto bit = b.begin();

  for (; ait != a.end() && bit != b.end(); ++ait, ++bit) {
    if (ait->loc != bit->loc) {
      return false;
    }
    if (abs((ait->dval-bit->dval)/ait->dval) > 10e-6) {
      return false;
    }
  }

  return (ait == a.end() && bit == b.end());
}

bool operator==(const TensorBase& l, const TensorBase& r) {
  return l.content == r.content;
}

bool operator<(const TensorBase& l, const TensorBase& r) {
  return l.content < r.content;
}

void TensorBase::assembleInternal() {
  content->module->callFunc("assemble", content->arguments.data());
  
  size_t j = 0;
  auto resultStorage = getStorage();
  auto resultFormat = resultStorage.getFormat();
  for (size_t i=0; i<resultFormat.getLevels().size(); i++) {
    Storage::LevelIndex& levelIndex = resultStorage.getLevelIndex(i);
    auto& levelFormat = resultFormat.getLevels()[i];
    switch (levelFormat.getType()) {
      case Dense:
        j++;
        break;
      case Sparse:
      case Fixed:
        levelIndex.ptr = (int*)content->arguments[j++];
        levelIndex.idx = (int*)content->arguments[j++];
        break;
    }
  }

  content->valuesSize = resultStorage.getSize().values;
  resultStorage.setValues((double*)malloc(content->valuesSize*sizeof(double)));
  content->arguments[j] = resultStorage.getValues();
}

void TensorBase::computeInternal() {
  this->content->module->callFunc("compute", content->arguments.data());
}

ostream& operator<<(ostream& os, const TensorBase& tensor) {
  vector<string> dimStrings;
  for (int dim : tensor.getDimensions()) {
    dimStrings.push_back(to_string(dim));
  }
  os << tensor.getName() << " (" << util::join(dimStrings, "x") << ") "
     << tensor.getFormat() << ":" << std::endl;

  // Print coordinates
  size_t numCoordinates = tensor.coordinateBufferUsed / tensor.coordinateSize;
  for (size_t i = 0; i < numCoordinates; i++) {
    int* ptr = (int*)&tensor.coordinateBuffer->data()[i*tensor.coordinateSize];
    os << "(" << util::join(ptr, ptr+tensor.getOrder()) << "): "
       << ((double*)(ptr+tensor.getOrder()))[0] << std::endl;
  }

  // Print packed data
  taco_iassert(tensor.getStorage().defined()) << "undefined storage";
  os << tensor.getStorage();

  return os;
}

static string getTensorName(string filename) {
  string name = filename.substr(filename.find_last_of("/") + 1);
  return filename.substr(name.find_first_of(".") + 1);
}

static string getExtension(string filename) {
  return filename.substr(filename.find_last_of(".") + 1);
}

template <typename T>
TensorBase dispatchRead(T& file, FileFormat fileFormat, string name) {
  TensorBase tensor;
  switch (fileFormat) {
    case FileFormat::dns:
      taco_not_supported_yet;
      break;
    case FileFormat::mtx:
      tensor = mtx::read(file, name);
      break;
    case FileFormat::tns:
      tensor = tns::read(file, name);
      break;
    case FileFormat::rb:
      taco_not_supported_yet;
      break;
  }
  return tensor;
}

TensorBase readTensor(std::string filename, std::string name) {
  string extension = getExtension(filename);
  name = (name != "") ? name : getTensorName(filename);

  TensorBase tensor;
  if (extension == "mtx") {
    tensor = dispatchRead(filename, FileFormat::mtx, name);
  }
  else if (extension == "tns") {
    tensor = dispatchRead(filename, FileFormat::tns, name);
  }
  else {
    taco_uerror << "File extension not recognized: " << filename << std::endl;
  }
  return tensor;
}

TensorBase readTensor(string filename, FileFormat format, string name) {
  return dispatchRead(filename, format, name);
}

TensorBase readTensor(istream& stream, FileFormat format, string name) {
  return dispatchRead(stream, format, name);}

template <typename T>
void dispatchWrite(T& file, const TensorBase& tensor, FileFormat fileFormat) {
  switch (fileFormat) {
    case FileFormat::dns:
      taco_not_supported_yet;
      break;
    case FileFormat::mtx:
      mtx::write(file, tensor);
      break;
    case FileFormat::tns:
      tns::write(file, tensor);
      break;
    case FileFormat::rb:
      taco_not_supported_yet;
      break;
  }
}

void writeTensor(string filename, const TensorBase& tensor) {
  string extension = getExtension(filename);
  if (extension == "mtx") {
    dispatchWrite(filename, tensor, FileFormat::mtx);
  }
  else if (extension == "tns") {
    dispatchWrite(filename, tensor, FileFormat::tns);
  }
  else {
    taco_uerror << "File extension not recognized: " << filename << std::endl;
  }
}

void writeTensor(string filename, FileFormat format, const TensorBase& tensor) {
  dispatchWrite(filename, tensor, format);
}

void writeTensor(ofstream& stream, FileFormat format, const TensorBase& tensor){
  dispatchWrite(stream, tensor, format);
}

void packOperands(const TensorBase& tensor) {
  for (TensorBase operand : getOperands(tensor.getExpr())) {
    operand.pack();
  }
}

}

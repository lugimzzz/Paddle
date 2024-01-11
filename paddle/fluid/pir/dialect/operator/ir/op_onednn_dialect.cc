// Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/pir/dialect/operator/ir/op_onednn_dialect.h"
#include "paddle/fluid/pir/dialect/operator/ir/control_flow_op.h"
#include "paddle/fluid/pir/dialect/operator/ir/op_attribute.h"
#include "paddle/fluid/pir/dialect/operator/ir/op_type.h"
#include "paddle/fluid/pir/dialect/operator/ir/pd_op.h"
#include "paddle/fluid/pir/dialect/operator/ir/type_storage.h"
#include "paddle/fluid/pir/dialect/operator/transforms/param_to_variable.h"
#include "paddle/pir/core/builtin_type_interfaces.h"
#include "paddle/pir/core/interface_value.h"
#include "paddle/pir/core/ir_printer.h"
#include "paddle/pir/core/utils.h"
#include "paddle/pir/dialect/control_flow/ir/cf_dialect.h"
#include "paddle/pir/dialect/control_flow/ir/cf_op.h"

#ifdef PADDLE_WITH_DNNL
#include "paddle/fluid/pir/dialect/operator/ir/onednn_op.h"
#endif

namespace paddle {
namespace dialect {

OneDNNOperatorDialect::OneDNNOperatorDialect(pir::IrContext *ctx)
    : pir::Dialect(name(), ctx, pir::TypeId::get<OneDNNOperatorDialect>()) {
  initialize();
}

void OneDNNOperatorDialect::initialize() {
  // NOTE(zhangbo9674): GET_OP_LIST is defined in pd_op.h which is
  // generated by op_gen.py, see details in
  // paddle/fluid/pir/dialect/CMakeLists.txt.
  // NOTE(Ruting)GET_MANUAL_OP_LIST is define in manual_op.h"
  // use RegisterOps when list has more than two ops.

  // NOTE(cocoshe): VS2017 has a limit on the length of template
  // parameters, which causes "fatal error C1202".
  // Split GET_OP_LIST into two part on WIN32 here.
#ifdef WIN32
  RegisterOps<
#define GET_OP_LIST1
#include "paddle/fluid/pir/dialect/operator/ir/pd_onednn_op_info.cc"  // NOLINT
      >();
  RegisterOps<
#define GET_OP_LIST2
#include "paddle/fluid/pir/dialect/operator/ir/pd_onednn_op_info.cc"  // NOLINT
      >();
#else
  RegisterOps<
#define GET_OP_LIST
#include "paddle/fluid/pir/dialect/operator/ir/pd_onednn_op_info.cc"  // NOLINT
      >();
#endif
}

void OneDNNOperatorDialect::PrintType(pir::Type type, std::ostream &os) const {
  os << type.dialect().name();
  os << '.';
  if (auto tensor_type = type.dyn_cast<DenseTensorType>()) {
    os << "tensor<";
    for (auto d : common::vectorize(tensor_type.dims())) {
      os << d;
      os << "x";
    }
    tensor_type.dtype().Print(os);
    os << ">";
  } else if (auto selected_rows_type = type.dyn_cast<SelectedRowsType>()) {
    os << "selectedrows<";
    for (auto d : common::vectorize(selected_rows_type.dims())) {
      os << d;
      os << "x";
    }
    selected_rows_type.dtype().Print(os);
    os << ">";
  } else if (auto tensor_array_type = type.dyn_cast<DenseTensorArrayType>()) {
    os << "tensor_array<";
    tensor_array_type.dtype().Print(os);
    os << ">";
  }
}

void OneDNNOperatorDialect::PrintAttribute(pir::Attribute attr,
                                           std::ostream &os) const {
  os << "(" << attr.dialect().name();
  os << '.';
  if (auto int_array_attr = attr.dyn_cast<IntArrayAttribute>()) {
    phi::IntArray data = int_array_attr.data();
    os << "IntArray)"
       << "[";
    const auto &inner_data = data.GetData();
    pir::PrintInterleave(
        inner_data.begin(),
        inner_data.end(),
        [&os](int64_t i) { os << i; },
        [&os]() { os << ","; });
    os << "]";
  } else if (auto data_type_attr = attr.dyn_cast<DataTypeAttribute>()) {
    os << "DataType)" << data_type_attr.data();
  } else if (auto place_type_attr = attr.dyn_cast<PlaceAttribute>()) {
    os << "Place)" << place_type_attr.data();
  } else if (auto data_layout_attr = attr.dyn_cast<DataLayoutAttribute>()) {
    os << "DataLayout)" << data_layout_attr.data();
  } else {
    os << "<#AttrNotImplemented>";
  }
}

pir::Type OneDNNOperatorDialect::ParseType(pir::IrParser &parser) {  // NOLINT
  parser.ConsumeAToken("pd_op.tensor");
  parser.ConsumeAToken("<");
  std::vector<int> dim{};
  Token dim_token = parser.PeekToken();
  while (dim_token.token_type_ == DIGIT) {
    dim_token = parser.ConsumeToken();
    dim.push_back(atoi(dim_token.val_.c_str()));
    std::string peek_token_val = parser.PeekToken().val_;
    if (peek_token_val[0] != 'x') {
      break;
    }
    parser.ConsumeToken();
    parser.lexer->Unget(static_cast<int>(peek_token_val.size() - 1));
    if (parser.PeekToken().token_type_ != DIGIT) {
      break;
    }
  }
  phi::DDim ddim = common::make_ddim(dim);
  pir::Type dtype = parser.ParseType();
  std::vector<std::vector<size_t>> lod;
  std::vector<size_t> lodv;
  lodv.push_back(0);
  lod.push_back(lodv);
  parser.ConsumeAToken(">");
  return DenseTensorType::get(
      parser.ctx, dtype, ddim, phi::DataLayout::UNDEFINED, lod, 0);
}

pir::Attribute OneDNNOperatorDialect::ParseAttribute(
    pir::IrParser &parser) {  // NOLINT
  std::string type_name = parser.ConsumeToken().val_;
  std::string attribute_name =
      type_name.substr(type_name.find('.') + 1, std::string::npos);
  parser.ConsumeAToken(")");
  if (attribute_name == "IntArray") {
    return IntArrayAttribute::Parse(parser);
  } else if (attribute_name == "DataType") {
    return DataTypeAttribute::Parse(parser);
  } else if (attribute_name == "Place") {
    return PlaceAttribute::Parse(parser);
  } else if (attribute_name == "DataLayout") {
    return DataLayoutAttribute::Parse(parser);
  } else {
    IR_THROW("No function to parse " + attribute_name + " exists!" +
             parser.GetErrorLocationInfo());
  }
}

void OneDNNOperatorDialect::PrintOperation(pir::Operation *op,
                                           pir::IrPrinter &printer) const {
  if (auto if_op = op->dyn_cast<IfOp>()) {
    if_op.Print(printer);
  } else if (auto while_op = op->dyn_cast<WhileOp>()) {
    while_op.Print(printer);
  } else {
    printer.PrintGeneralOperation(op);
  }
}

}  // namespace dialect
}  // namespace paddle

IR_DEFINE_EXPLICIT_TYPE_ID(paddle::dialect::OneDNNOperatorDialect)

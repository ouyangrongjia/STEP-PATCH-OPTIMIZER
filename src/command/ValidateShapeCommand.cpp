#include "command/ValidateShapeCommand.h"

#include "command/CommandContext.h"
#include "validate/ShapeValidator.h"

namespace spo {

const char* ValidateShapeCommand::name() const {
    return "ValidateShapeCommand";
}

Result ValidateShapeCommand::execute(CommandContext& context) {
    ShapeValidator validator;
    context.validationReport = validator.validate(context.document);
    if (!context.validationReport.has_shape) {
        return Result::error("当前没有已加载的模型。");
    }
    return Result::ok();
}

}

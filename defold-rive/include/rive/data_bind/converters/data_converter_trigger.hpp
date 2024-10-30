#ifndef _RIVE_DATA_CONVERTER_TRIGGER_HPP_
#define _RIVE_DATA_CONVERTER_TRIGGER_HPP_
#include "rive/generated/data_bind/converters/data_converter_trigger_base.hpp"
#include <stdio.h>
namespace rive
{
class DataConverterTrigger : public DataConverterTriggerBase
{
public:
    DataValue* convert(DataValue* value) override;
    DataType outputType() override { return DataType::trigger; };
};
} // namespace rive

#endif
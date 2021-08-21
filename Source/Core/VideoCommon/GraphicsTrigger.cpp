// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/GraphicsTrigger.h"

#include <fmt/format.h>

#include "Common/IniFile.h"
#include "Common/VariantUtil.h"

bool LoadFromFile(GraphicsTrigger& trigger, const std::string& file)
{
  IniFile ini_file;
  ini_file.Load(file);
  IniFile::Section* section = ini_file.GetOrCreateSection("Trigger");

  std::string type;
  if (!section->Get("Type", &type))
    return false;

  const auto load_width_height_format = [&](auto& trigger) -> bool {
    u32 width_operation;
    if (!section->Get("WidthOperation", &width_operation))
      return false;
    trigger.width_operation = static_cast<NumericOperation>(width_operation);
    if (trigger.width_operation != NumericOperation::Any)
    {
      if (!section->Get("Width", &trigger.width))
        return false;
    }

    u32 height_operation;
    if (!section->Get("HeightOperation", &height_operation))
      return false;
    trigger.height_operation = static_cast<NumericOperation>(height_operation);
    if (trigger.height_operation != NumericOperation::Any)
    {
      if (!section->Get("Height", &trigger.height))
        return false;
    }

    u32 format_operation;
    if (!section->Get("FormatOperation", &format_operation))
      return false;
    trigger.format_operation = static_cast<MultiGenericOperation>(format_operation);
    if (trigger.format_operation != MultiGenericOperation::Any)
    {
      if (trigger.format_operation == MultiGenericOperation::Exact)
      {
        TextureFormat format;
        if (!section->Get("Format", &format))
          return false;
        trigger.formats.push_back(format);
      }
      else
      {
        std::string delimited_format;
        if (!section->Get("Format", &delimited_format))
          return false;
        const auto formats = SplitString(delimited_format, ',');
        for (const auto& format_str : formats)
        {
          TextureFormat format;
          if (!TryParse(format_str, &format))
          {
            return false;
          }
          trigger.formats.push_back(format);
        }
      }
    }

    return true;
  };

  const auto load_hash_tlut = [&](auto& trigger) -> bool {
    u32 tlut_operation;
    if (!section->Get("TlutOperation", &tlut_operation))
      return false;
    trigger.tlut_operation = static_cast<GenericOperation>(tlut_operation);
    if (trigger.tlut_operation != GenericOperation::Any)
    {
      if (!section->Get("Tlut", &trigger.tlut))
        return false;
    }

    u32 hash_operation;
    if (!section->Get("HashOperation", &hash_operation))
      return false;
    trigger.hash_operation = static_cast<GenericOperation>(hash_operation);
    if (trigger.hash_operation != GenericOperation::Any)
    {
      if (!section->Get("Hash", &trigger.hash))
        return false;
    }

    return true;
  };

  if (type == "EFB")
  {
    EFBGraphicsTrigger efb_trigger;
    if (!load_width_height_format(efb_trigger))
      return false;

    trigger = efb_trigger;
    return true;
  }
  else if (type == "DrawCall2D")
  {
    DrawCall2DGraphicsTrigger drawcall2d_trigger;
    if (!load_width_height_format(drawcall2d_trigger))
      return false;
    if (!load_hash_tlut(drawcall2d_trigger))
      return false;

    trigger = drawcall2d_trigger;
    return true;
  }
  else if (type == "DrawCall3D")
  {
    DrawCall3DGraphicsTrigger drawcall3d_trigger;
    if (!load_width_height_format(drawcall3d_trigger))
      return false;
    if (!load_hash_tlut(drawcall3d_trigger))
      return false;

    trigger = drawcall3d_trigger;
    return true;
  }
  else if (type == "TextureLoad")
  {
    TextureLoadGraphicsTrigger textureload_trigger;
    if (!section->Get("TextureId", &textureload_trigger.texture_id))
      return false;

    trigger = textureload_trigger;
    return true;
  }
  else if (type == "Post")
  {
    return true;
  }

  return false;
}

void SaveToFile(const GraphicsTrigger& trigger, const std::string& file)
{
  IniFile ini_file;
  IniFile::Section* section = ini_file.GetOrCreateSection("Trigger");

  const auto save_width_height_format = [&](const auto& trigger) {
    section->Set("WidthOperation", trigger.width_operation);
    if (trigger.width_operation != NumericOperation::Any)
    {
      section->Set("Width", trigger.width);
    }

    section->Set("HeightOperation", trigger.height_operation);
    if (trigger.height_operation != NumericOperation::Any)
    {
      section->Set("Height", trigger.height);
    }

    section->Set("FormatOperation", trigger.format_operation);
    if (trigger.format_operation != MultiGenericOperation::Any)
    {
      section->Set("Format", fmt::to_string(fmt::join(trigger.formats, ",")));
    }
  };

  const auto save_hash_tlut = [&](const auto& trigger) {
    section->Set("HashOperation", trigger.hash_operation);
    if (trigger.hash_operation != GenericOperation::Any)
    {
      section->Set("Hash", trigger.hash);
    }

    section->Set("TlutOperation", trigger.tlut_operation);
    if (trigger.tlut_operation != GenericOperation::Any)
    {
      section->Set("Tlut", trigger.tlut);
    }
  };

  std::visit(overloaded{[&](const EFBGraphicsTrigger& t) {
                          section->Set("Type", "EFB");
                          save_width_height_format(t);
                        },
                        [&](const TextureLoadGraphicsTrigger& t) {
                          section->Set("Type", "TextureLoad");
                          section->Set("TextureId", t.texture_id);
                        },
                        [&](const DrawCall2DGraphicsTrigger& t) {
                          section->Set("Type", "DrawCall2D");
                          save_width_height_format(t);
                          save_hash_tlut(t);
                        },
                        [&](const DrawCall3DGraphicsTrigger& t) {
                          section->Set("Type", "DrawCall3D");
                          save_width_height_format(t);
                          save_hash_tlut(t);
                        },
                        [&](const PostGraphicsTrigger& t) { section->Set("Type", "Post"); }},
             trigger);
}

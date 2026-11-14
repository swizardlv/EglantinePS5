// SPDX-License-Identifier: GPL-3.0-or-later

#include <initializer_list>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"

namespace {
constexpr u32 OpCapability = 17;
constexpr u32 OpDecorate = 71;
constexpr u32 DecorationBuiltIn = 11;
constexpr u32 BuiltInPrimitiveId = 7;
constexpr u32 BuiltInBaseVertex = 4424;
constexpr u32 BuiltInBaseInstance = 4425;
constexpr u32 BuiltInDrawIndex = 4426;
constexpr u32 CapabilityGeometry = 2;
constexpr u32 CapabilityDrawParameters = 4427;

std::vector<u32> Emit(Shader::Stage stage, bool support_vertex_instance_id,
                      std::initializer_list<Shader::IR::Attribute> loads) {
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::IR::Block block{inst_pool};
    Shader::IR::Program program;
    program.stage = stage;
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    Shader::IR::AbstractSyntaxNode block_node{};
    block_node.type = Shader::IR::AbstractSyntaxNode::Type::Block;
    block_node.data.block = &block;
    Shader::IR::AbstractSyntaxNode return_node{};
    return_node.type = Shader::IR::AbstractSyntaxNode::Type::Return;
    program.syntax_list = {block_node, return_node};
    for (const Shader::IR::Attribute attribute : loads) {
        program.info.loads.Set(attribute);
    }
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_vertex_instance_id = support_vertex_instance_id;
    return Shader::Backend::SPIRV::EmitSPIRV(profile, program);
}

bool HasCapability(const std::vector<u32>& code, u32 capability) {
    for (size_t offset = 5; offset < code.size();) {
        const u32 word_count = code[offset] >> 16;
        if (word_count == 0) {
            break;
        }
        if ((code[offset] & 0xffff) == OpCapability && code[offset + 1] == capability) {
            return true;
        }
        offset += word_count;
    }
    return false;
}

bool HasBuiltIn(const std::vector<u32>& code, u32 builtin) {
    for (size_t offset = 5; offset < code.size();) {
        const u32 word_count = code[offset] >> 16;
        if (word_count == 0) {
            break;
        }
        if ((code[offset] & 0xffff) == OpDecorate && word_count >= 4 &&
            code[offset + 2] == DecorationBuiltIn && code[offset + 3] == builtin) {
            return true;
        }
        offset += word_count;
    }
    return false;
}
} // Anonymous namespace

TEST_CASE("SPIR-V draw parameter built-ins declare DrawParameters",
          "[video_core][spirv][draw_parameters]") {
    using Shader::IR::Attribute;
    struct Case {
        bool support_vertex_instance_id;
        Attribute load;
        u32 builtin;
    };
    for (const Case test : {
             Case{true, Attribute::BaseInstance, BuiltInBaseInstance},
             Case{true, Attribute::BaseVertex, BuiltInBaseVertex},
             Case{true, Attribute::DrawID, BuiltInDrawIndex},
             Case{false, Attribute::BaseInstance, BuiltInBaseInstance},
             Case{false, Attribute::BaseVertex, BuiltInBaseVertex},
             Case{false, Attribute::DrawID, BuiltInDrawIndex},
             Case{false, Attribute::InstanceId, BuiltInBaseInstance},
             Case{false, Attribute::VertexId, BuiltInBaseVertex},
         }) {
        const std::vector<u32> code =
            Emit(Shader::Stage::VertexB, test.support_vertex_instance_id, {test.load});
        REQUIRE(HasBuiltIn(code, test.builtin));
        REQUIRE(HasCapability(code, CapabilityDrawParameters));
    }
}

TEST_CASE("SPIR-V vertex and instance ids alone do not need DrawParameters",
          "[video_core][spirv][draw_parameters]") {
    using Shader::IR::Attribute;
    const std::vector<u32> code =
        Emit(Shader::Stage::VertexB, true, {Attribute::InstanceId, Attribute::VertexId});
    REQUIRE_FALSE(HasBuiltIn(code, BuiltInBaseInstance));
    REQUIRE_FALSE(HasBuiltIn(code, BuiltInBaseVertex));
    REQUIRE_FALSE(HasCapability(code, CapabilityDrawParameters));
}

TEST_CASE("SPIR-V fragment PrimitiveId declares Geometry", "[video_core][spirv][primitive_id]") {
    using Shader::IR::Attribute;
    const std::vector<u32> with_id =
        Emit(Shader::Stage::Fragment, false, {Attribute::PrimitiveId});
    REQUIRE(HasBuiltIn(with_id, BuiltInPrimitiveId));
    REQUIRE(HasCapability(with_id, CapabilityGeometry));

    const std::vector<u32> without_id = Emit(Shader::Stage::Fragment, false, {});
    REQUIRE_FALSE(HasBuiltIn(without_id, BuiltInPrimitiveId));
    REQUIRE_FALSE(HasCapability(without_id, CapabilityGeometry));
}

TEST_CASE("SPIR-V workgroup built-ins reject non-workgroup stages",
          "[video_core][spirv][workgroup_stage]") {
    using Shader::Stage;
    for (const Stage stage : {Stage::VertexA, Stage::VertexB, Stage::TessellationControl,
                              Stage::TessellationEval, Stage::Geometry, Stage::Fragment,
                              Stage::Compute, Stage::Task, Stage::Mesh}) {
        for (const unsigned flags : {0U, 1U, 2U, 3U}) {
            CAPTURE(stage, flags);
            Shader::IR::Program program{};
            program.stage = stage;
            program.info.uses_workgroup_id = (flags & 1U) != 0;
            program.info.uses_local_invocation_id = (flags & 2U) != 0;
            program.workgroup_size = {1, 1, 1};
            Shader::Profile profile{};
            profile.supported_spirv = 0x00010600;
            Shader::RuntimeInfo runtime{};
            Shader::Backend::Bindings bindings{};
            const auto emit = [&] {
                Shader::Backend::SPIRV::EmitContext context(profile, runtime, program, bindings);
            };
            if (flags != 0 && stage != Stage::Compute && stage != Stage::Task &&
                stage != Stage::Mesh) {
                REQUIRE_THROWS_AS(emit(), Shader::NotImplementedException);
            } else {
                REQUIRE_NOTHROW(emit());
            }
        }
    }
}

#ifndef SPIRV_MSL_ATOMIC_H
#define SPIRV_MSL_ATOMIC_H

#include <spirv_msl.hpp>

// Apple exposes no-return device ulong min/max from MSL 2.4. Keep this narrow
// adapter outside the bundled SPIRV-Cross sources. Other 64-bit atomic operations
// still follow its explicit refusal, including any request for the old value.
class CompilerMSLAtomic : public spirv_cross::CompilerMSL {
	static constexpr const char *UNAVAILABLE_RESULT = "spvVeyaAtomic64ReturnValueIsUnavailable";

protected:
	void emit_instruction(const spirv_cross::Instruction &p_instruction) override {
		using namespace spirv_cross;
		const auto opcode = static_cast<spv::Op>(p_instruction.op);
		if (opcode != spv::OpAtomicUMax && opcode != spv::OpAtomicUMin) {
			CompilerMSL::emit_instruction(p_instruction);
			return;
		}
		const uint32_t *operands = stream(p_instruction);
		const auto &result_type = get<SPIRType>(operands[0]);
		if (result_type.width != 64) {
			CompilerMSL::emit_instruction(p_instruction);
			return;
		}
		const auto &pointer_type = expression_type(operands[2]);
		const auto &scope = get<SPIRConstant>(operands[3]);
		const auto &semantics = get<SPIRConstant>(operands[4]);
		if (!get_msl_options().supports_msl_version(2, 4) ||
				result_type.basetype != SPIRType::UInt64 || result_type.vecsize != 1 ||
				(pointer_type.storage != spv::StorageClassStorageBuffer && pointer_type.storage != spv::StorageClassUniform) ||
				scope.specialization || semantics.specialization || scope.scalar() != spv::ScopeDevice ||
				semantics.scalar() != spv::MemorySemanticsMaskNone) {
			SPIRV_CROSS_THROW("64-bit buffer min/max requires MSL 2.4, device scope and relaxed memory order.");
		}
		if (needs_frag_discard_checks()) {
			statement("if (!", builtin_to_glsl(spv::BuiltInHelperInvocation, spv::StorageClassInput), ")");
			begin_scope();
		}
		statement(opcode == spv::OpAtomicUMax ? "atomic_max_explicit" : "atomic_min_explicit",
				"((volatile device atomic_ulong *)&", to_enclosed_expression(operands[2]), ", ",
				to_expression(operands[5]), ", memory_order_relaxed);");
		if (needs_frag_discard_checks()) {
			end_scope();
		}
		// This expression appears in generated source only if a consumer reads the
		// old value. Reject that source before it reaches a Metal library or cache.
		emit_op(operands[0], operands[1], UNAVAILABLE_RESULT, true);
		flush_all_atomic_capable_variables();
	}

public:
	using spirv_cross::CompilerMSL::CompilerMSL;

	std::string compile() override {
		using spirv_cross::CompilerError;
		std::string source = CompilerMSL::compile();
		if (source.find(UNAVAILABLE_RESULT) != std::string::npos) {
			SPIRV_CROSS_THROW("Metal 64-bit buffer min/max cannot return the old value.");
		}
		return source;
	}
};

#endif // SPIRV_MSL_ATOMIC_H

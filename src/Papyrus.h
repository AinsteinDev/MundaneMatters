#pragma once

// Calls Papyrus functions from C++ through the script VM, so vanilla logic runs exactly as
// it does from a dialogue fragment (Actor.ShowBarterMenu, Game.ShowTrainingMenu, ...).
// Call these on the game thread (inside SKSE::GetTaskInterface()->AddTask).
namespace MundaneMatters::Papyrus
{
	using Result = std::function<void(const RE::BSScript::Variable&)>;

	namespace detail
	{
		class Callback : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit Callback(Result fn) :
				_fn(std::move(fn)) {}

			void operator()(RE::BSScript::Variable a_result) override
			{
				if (_fn) {
					_fn(a_result);
				}
			}
			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

		private:
			Result _fn;
		};

		inline RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> MakeCallback(Result fn)
		{
			return RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>{ new Callback(std::move(fn)) };
		}
	}

	template <class... Args>
	bool CallStatic(const char* scriptName, const char* fnName, Result onResult, Args&&... args)
	{
		auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			return false;
		}
		auto cb = detail::MakeCallback(std::move(onResult));
		return vm->DispatchStaticCall(scriptName, fnName, RE::MakeFunctionArguments(std::forward<Args>(args)...), cb);
	}

	// Calls a member function of a script bound to the form (e.g. "Actor"/"ShowBarterMenu" on an NPC).
	template <class... Args>
	bool CallMethod(RE::TESForm* form, const char* scriptName, const char* fnName, Result onResult, Args&&... args)
	{
		auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm || !form) {
			return false;
		}
		auto* policy = vm->GetObjectHandlePolicy();
		auto handle = policy->GetHandleForObject(form->GetFormType(), form);
		if (handle == policy->EmptyHandle()) {
			return false;
		}
		auto cb = detail::MakeCallback(std::move(onResult));
		return vm->DispatchMethodCall(handle, scriptName, fnName, RE::MakeFunctionArguments(std::forward<Args>(args)...), cb);
	}

	// The script instance attached to the form (e.g. "RentRoomScript" on innkeepers), or null.
	inline RE::BSTSmartPointer<RE::BSScript::Object> GetScript(RE::TESForm* form, const char* scriptName)
	{
		RE::BSTSmartPointer<RE::BSScript::Object> obj;
		auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm || !form) {
			return obj;
		}
		auto* policy = vm->GetObjectHandlePolicy();
		auto handle = policy->GetHandleForObject(form->GetFormType(), form);
		if (handle != policy->EmptyHandle()) {
			vm->FindBoundObject(handle, scriptName, obj);
		}
		return obj;
	}

	inline bool HasScript(RE::TESForm* form, const char* scriptName)
	{
		return static_cast<bool>(GetScript(form, scriptName));
	}

	// A form-typed script property (e.g. RentRoomScript.Bed), or null when unset.
	template <class T>
	T* FormProperty(const RE::BSTSmartPointer<RE::BSScript::Object>& obj, const char* name)
	{
		if (!obj) {
			return nullptr;
		}
		auto* var = obj->GetProperty(name);
		if (!var || var->IsNoneObject() || !var->IsObject()) {
			return nullptr;
		}
		return var->Unpack<T*>();
	}

	// Arguments built from ready-made Variables, for parameters typed as a script
	// (RentRoom(DialogueGenericScript) needs the quest's script object, not the bare Quest).
	class VarArgs : public RE::BSScript::IFunctionArguments
	{
	public:
		explicit VarArgs(std::vector<RE::BSScript::Variable> vars) :
			_vars(std::move(vars)) {}

		bool operator()(RE::BSScrapArray<RE::BSScript::Variable>& a_dst) const override
		{
			a_dst.resize(static_cast<std::uint32_t>(_vars.size()));
			for (std::size_t i = 0; i < _vars.size(); ++i) {
				a_dst[static_cast<std::uint32_t>(i)] = _vars[i];
			}
			return true;
		}

	private:
		std::vector<RE::BSScript::Variable> _vars;
	};

	inline bool CallMethodWith(RE::TESForm* form, const char* scriptName, const char* fnName, Result onResult, std::vector<RE::BSScript::Variable> args)
	{
		auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm || !form) {
			return false;
		}
		auto* policy = vm->GetObjectHandlePolicy();
		auto handle = policy->GetHandleForObject(form->GetFormType(), form);
		if (handle == policy->EmptyHandle()) {
			return false;
		}
		auto cb = detail::MakeCallback(std::move(onResult));
		return vm->DispatchMethodCall(handle, scriptName, fnName, new VarArgs(std::move(args)), cb);
	}
}

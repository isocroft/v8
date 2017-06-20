// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/runtime/runtime-utils.h"

#include "src/allocation-site-scopes.h"
#include "src/arguments.h"
#include "src/ast/ast.h"
#include "src/ast/compile-time-value.h"
#include "src/isolate-inl.h"
#include "src/runtime/runtime.h"

namespace v8 {
namespace internal {

namespace {

bool IsUninitializedLiteralSite(Handle<Object> literal_site) {
  return *literal_site == Smi::kZero;
}

bool HasBoilerplate(Isolate* isolate, Handle<Object> literal_site) {
  return !literal_site->IsSmi();
}

void PreInitializeLiteralSite(Handle<FeedbackVector> vector,
                              FeedbackSlot slot) {
  vector->Set(slot, Smi::FromInt(1));
}

Handle<Object> InnerCreateBoilerplate(Isolate* isolate,
                                      Handle<FeedbackVector> vector,
                                      Handle<FixedArray> compile_time_value);

struct ObjectBoilerplate {
  static Handle<JSObject> Create(Isolate* isolate,
                                 Handle<FeedbackVector> vector,
                                 Handle<HeapObject> description, int flags) {
    Handle<Context> native_context = isolate->native_context();
    Handle<BoilerplateDescription> boilerplate_description =
        Handle<BoilerplateDescription>::cast(description);
    bool use_fast_elements = (flags & ObjectLiteral::kFastElements) != 0;
    bool has_null_prototype = (flags & ObjectLiteral::kHasNullPrototype) != 0;

    // In case we have function literals, we want the object to be in
    // slow properties mode for now. We don't go in the map cache because
    // maps with constant functions can't be shared if the functions are
    // not the same (which is the common case).
    int number_of_properties = boilerplate_description->backing_store_size();

    // Ignoring number_of_properties for force dictionary map with
    // __proto__:null.
    Handle<Map> map =
        has_null_prototype
            ? handle(native_context->slow_object_with_null_prototype_map(),
                     isolate)
            : isolate->factory()->ObjectLiteralMapFromCache(
                  native_context, number_of_properties);

    PretenureFlag pretenure_flag =
        isolate->heap()->InNewSpace(*vector) ? NOT_TENURED : TENURED;

    Handle<JSObject> boilerplate =
        map->is_dictionary_map()
            ? isolate->factory()->NewSlowJSObjectFromMap(
                  map, number_of_properties, pretenure_flag)
            : isolate->factory()->NewJSObjectFromMap(map, pretenure_flag);

    // Normalize the elements of the boilerplate to save space if needed.
    if (!use_fast_elements) JSObject::NormalizeElements(boilerplate);

    // Add the constant properties to the boilerplate.
    int length = boilerplate_description->size();
    // TODO(verwaest): Support tracking representations in the boilerplate.
    for (int index = 0; index < length; index++) {
      Handle<Object> key(boilerplate_description->name(index), isolate);
      Handle<Object> value(boilerplate_description->value(index), isolate);
      if (value->IsFixedArray()) {
        // The value contains the CompileTimeValue with the boilerplate
        // properties of a simple object or array literal.
        Handle<FixedArray> compile_time_value = Handle<FixedArray>::cast(value);
        value = InnerCreateBoilerplate(isolate, vector, compile_time_value);
      }
      uint32_t element_index = 0;
      if (key->ToArrayIndex(&element_index)) {
        // Array index (uint32).
        if (value->IsUninitialized(isolate)) {
          value = handle(Smi::kZero, isolate);
        }
        JSObject::SetOwnElementIgnoreAttributes(boilerplate, element_index,
                                                value, NONE)
            .Check();
      } else {
        Handle<String> name = Handle<String>::cast(key);
        DCHECK(!name->AsArrayIndex(&element_index));
        JSObject::SetOwnPropertyIgnoreAttributes(boilerplate, name, value, NONE)
            .Check();
      }
    }

    if (map->is_dictionary_map() && !has_null_prototype) {
      // TODO(cbruni): avoid making the boilerplate fast again, the clone stub
      // supports dict-mode objects directly.
      JSObject::MigrateSlowToFast(boilerplate,
                                  boilerplate->map()->unused_property_fields(),
                                  "FastLiteral");
    }
    return boilerplate;
  }
};

struct ArrayBoilerplate {
  static Handle<JSObject> Create(Isolate* isolate,
                                 Handle<FeedbackVector> vector,
                                 Handle<HeapObject> description, int flags) {
    Handle<ConstantElementsPair> elements =
        Handle<ConstantElementsPair>::cast(description);
    // Create the JSArray.
    ElementsKind constant_elements_kind =
        static_cast<ElementsKind>(elements->elements_kind());

    Handle<FixedArrayBase> constant_elements_values(
        elements->constant_values());
    Handle<FixedArrayBase> copied_elements_values;
    if (IsFastDoubleElementsKind(constant_elements_kind)) {
      copied_elements_values = isolate->factory()->CopyFixedDoubleArray(
          Handle<FixedDoubleArray>::cast(constant_elements_values));
    } else {
      DCHECK(IsFastSmiOrObjectElementsKind(constant_elements_kind));
      const bool is_cow = (constant_elements_values->map() ==
                           isolate->heap()->fixed_cow_array_map());
      if (is_cow) {
        copied_elements_values = constant_elements_values;
#if DEBUG
        Handle<FixedArray> fixed_array_values =
            Handle<FixedArray>::cast(copied_elements_values);
        for (int i = 0; i < fixed_array_values->length(); i++) {
          DCHECK(!fixed_array_values->get(i)->IsFixedArray());
        }
#endif
      } else {
        Handle<FixedArray> fixed_array_values =
            Handle<FixedArray>::cast(constant_elements_values);
        Handle<FixedArray> fixed_array_values_copy =
            isolate->factory()->CopyFixedArray(fixed_array_values);
        copied_elements_values = fixed_array_values_copy;
        FOR_WITH_HANDLE_SCOPE(
            isolate, int, i = 0, i, i < fixed_array_values->length(), i++, {
              if (fixed_array_values->get(i)->IsFixedArray()) {
                // The value contains the CompileTimeValue with the
                // boilerplate description of a simple object or
                // array literal.
                Handle<FixedArray> compile_time_value(
                    FixedArray::cast(fixed_array_values->get(i)));
                Handle<Object> result =
                    InnerCreateBoilerplate(isolate, vector, compile_time_value);
                fixed_array_values_copy->set(i, *result);
              }
            });
      }
    }

    PretenureFlag pretenure_flag =
        isolate->heap()->InNewSpace(*vector) ? NOT_TENURED : TENURED;
    return isolate->factory()->NewJSArrayWithElements(
        copied_elements_values, constant_elements_kind,
        copied_elements_values->length(), pretenure_flag);
  }
};

Handle<Object> InnerCreateBoilerplate(Isolate* isolate,
                                      Handle<FeedbackVector> vector,
                                      Handle<FixedArray> compile_time_value) {
  Handle<HeapObject> elements =
      CompileTimeValue::GetElements(compile_time_value);
  int flags = CompileTimeValue::GetLiteralTypeFlags(compile_time_value);
  if (flags == CompileTimeValue::kArrayLiteralFlag) {
    return ArrayBoilerplate::Create(isolate, vector, elements, flags);
  }
  return ObjectBoilerplate::Create(isolate, vector, elements, flags);
}

template <typename Boilerplate>
MaybeHandle<JSObject> CreateLiteral(Isolate* isolate,
                                    Handle<JSFunction> closure,
                                    int literals_index,
                                    Handle<HeapObject> description, int flags) {
  Handle<FeedbackVector> vector(closure->feedback_vector(), isolate);
  FeedbackSlot literals_slot(FeedbackVector::ToSlot(literals_index));
  CHECK(literals_slot.ToInt() < vector->slot_count());
  Handle<Object> literal_site(vector->Get(literals_slot), isolate);

  STATIC_ASSERT(static_cast<int>(ObjectLiteral::kShallowProperties) ==
                static_cast<int>(ArrayLiteral::kShallowElements));
  JSObject::DeepCopyHints copy_hints =
      (flags & ObjectLiteral::kShallowProperties) ? JSObject::kObjectIsShallow
                                                  : JSObject::kNoHints;
  if (FLAG_track_double_fields && !FLAG_unbox_double_fields) {
    // Make sure we properly clone mutable heap numbers on 32-bit platforms.
    copy_hints = JSObject::kNoHints;
  }

  Handle<AllocationSite> site;
  Handle<JSObject> boilerplate;

  if (HasBoilerplate(isolate, literal_site)) {
    site = Handle<AllocationSite>::cast(literal_site);
    boilerplate =
        Handle<JSObject>(JSObject::cast(site->transition_info()), isolate);
  } else {
    // Instantiate a JSArray or JSObject literal from the given {description}.
    boilerplate = Boilerplate::Create(isolate, vector, description, flags);
    if (IsUninitializedLiteralSite(literal_site)) {
      PreInitializeLiteralSite(vector, literals_slot);
      if (copy_hints == JSObject::kNoHints) {
        DeprecationUpdateContext update_context(isolate);
        RETURN_ON_EXCEPTION(isolate,
                            JSObject::DeepWalk(boilerplate, &update_context),
                            JSObject);
      }
      return boilerplate;
    }
    // Install AllocationSite objects.
    AllocationSiteCreationContext creation_context(isolate);
    site = creation_context.EnterNewScope();
    RETURN_ON_EXCEPTION(
        isolate, JSObject::DeepWalk(boilerplate, &creation_context), JSObject);
    creation_context.ExitScope(site, boilerplate);

    vector->Set(literals_slot, *site);
  }

  STATIC_ASSERT(static_cast<int>(ObjectLiteral::kDisableMementos) ==
                static_cast<int>(ArrayLiteral::kDisableMementos));
  bool enable_mementos = (flags & ObjectLiteral::kDisableMementos) == 0;

  // Copy the existing boilerplate.
  AllocationSiteUsageContext usage_context(isolate, site, enable_mementos);
  usage_context.EnterNewScope();
  MaybeHandle<JSObject> copy =
      JSObject::DeepCopy(boilerplate, &usage_context, copy_hints);
  usage_context.ExitScope(site, boilerplate);
  return copy;
}
}  // namespace

RUNTIME_FUNCTION(Runtime_CreateObjectLiteral) {
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, closure, 0);
  CONVERT_SMI_ARG_CHECKED(literals_index, 1);
  CONVERT_ARG_HANDLE_CHECKED(BoilerplateDescription, description, 2);
  CONVERT_SMI_ARG_CHECKED(flags, 3);
  RETURN_RESULT_OR_FAILURE(
      isolate, CreateLiteral<ObjectBoilerplate>(
                   isolate, closure, literals_index, description, flags));
}

RUNTIME_FUNCTION(Runtime_CreateArrayLiteral) {
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, closure, 0);
  CONVERT_SMI_ARG_CHECKED(literals_index, 1);
  CONVERT_ARG_HANDLE_CHECKED(ConstantElementsPair, elements, 2);
  CONVERT_SMI_ARG_CHECKED(flags, 3);
  RETURN_RESULT_OR_FAILURE(
      isolate, CreateLiteral<ArrayBoilerplate>(isolate, closure, literals_index,
                                               elements, flags));
}

RUNTIME_FUNCTION(Runtime_CreateRegExpLiteral) {
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, closure, 0);
  CONVERT_SMI_ARG_CHECKED(index, 1);
  CONVERT_ARG_HANDLE_CHECKED(String, pattern, 2);
  CONVERT_SMI_ARG_CHECKED(flags, 3);

  Handle<FeedbackVector> vector(closure->feedback_vector(), isolate);
  FeedbackSlot literal_slot(FeedbackVector::ToSlot(index));

  // Check if boilerplate exists. If not, create it first.
  Handle<Object> literal_site(vector->Get(literal_slot), isolate);
  Handle<Object> boilerplate;
  if (!HasBoilerplate(isolate, literal_site)) {
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
        isolate, boilerplate, JSRegExp::New(pattern, JSRegExp::Flags(flags)));
    if (IsUninitializedLiteralSite(literal_site)) {
      PreInitializeLiteralSite(vector, literal_slot);
      return *boilerplate;
    }
    vector->Set(literal_slot, *boilerplate);
  }
  return *JSRegExp::Copy(Handle<JSRegExp>::cast(boilerplate));
}

}  // namespace internal
}  // namespace v8

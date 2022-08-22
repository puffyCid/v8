#!/usr/bin/env python3
# Copyright 2019 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can
# be found in the LICENSE file.

# This file is automatically generated by mkgrokdump and should not
# be modified manually.

# List of known V8 instance types.
# yapf: disable

INSTANCE_TYPES = {
  0: "INTERNALIZED_STRING_TYPE",
  2: "EXTERNAL_INTERNALIZED_STRING_TYPE",
  8: "ONE_BYTE_INTERNALIZED_STRING_TYPE",
  10: "EXTERNAL_ONE_BYTE_INTERNALIZED_STRING_TYPE",
  18: "UNCACHED_EXTERNAL_INTERNALIZED_STRING_TYPE",
  26: "UNCACHED_EXTERNAL_ONE_BYTE_INTERNALIZED_STRING_TYPE",
  32: "STRING_TYPE",
  33: "CONS_STRING_TYPE",
  34: "EXTERNAL_STRING_TYPE",
  35: "SLICED_STRING_TYPE",
  37: "THIN_STRING_TYPE",
  40: "ONE_BYTE_STRING_TYPE",
  41: "CONS_ONE_BYTE_STRING_TYPE",
  42: "EXTERNAL_ONE_BYTE_STRING_TYPE",
  43: "SLICED_ONE_BYTE_STRING_TYPE",
  45: "THIN_ONE_BYTE_STRING_TYPE",
  50: "UNCACHED_EXTERNAL_STRING_TYPE",
  58: "UNCACHED_EXTERNAL_ONE_BYTE_STRING_TYPE",
  96: "SHARED_STRING_TYPE",
  101: "SHARED_THIN_STRING_TYPE",
  104: "SHARED_ONE_BYTE_STRING_TYPE",
  109: "SHARED_THIN_ONE_BYTE_STRING_TYPE",
  128: "SYMBOL_TYPE",
  129: "BIG_INT_BASE_TYPE",
  130: "HEAP_NUMBER_TYPE",
  131: "ODDBALL_TYPE",
  132: "PROMISE_FULFILL_REACTION_JOB_TASK_TYPE",
  133: "PROMISE_REJECT_REACTION_JOB_TASK_TYPE",
  134: "CALLABLE_TASK_TYPE",
  135: "CALLBACK_TASK_TYPE",
  136: "PROMISE_RESOLVE_THENABLE_JOB_TASK_TYPE",
  137: "LOAD_HANDLER_TYPE",
  138: "STORE_HANDLER_TYPE",
  139: "FUNCTION_TEMPLATE_INFO_TYPE",
  140: "OBJECT_TEMPLATE_INFO_TYPE",
  141: "ACCESS_CHECK_INFO_TYPE",
  142: "ACCESSOR_PAIR_TYPE",
  143: "ALIASED_ARGUMENTS_ENTRY_TYPE",
  144: "ALLOCATION_MEMENTO_TYPE",
  145: "ALLOCATION_SITE_TYPE",
  146: "ARRAY_BOILERPLATE_DESCRIPTION_TYPE",
  147: "ASM_WASM_DATA_TYPE",
  148: "ASYNC_GENERATOR_REQUEST_TYPE",
  149: "BREAK_POINT_TYPE",
  150: "BREAK_POINT_INFO_TYPE",
  151: "CACHED_TEMPLATE_OBJECT_TYPE",
  152: "CALL_SITE_INFO_TYPE",
  153: "CLASS_POSITIONS_TYPE",
  154: "DEBUG_INFO_TYPE",
  155: "ENUM_CACHE_TYPE",
  156: "ERROR_STACK_DATA_TYPE",
  157: "FEEDBACK_CELL_TYPE",
  158: "FUNCTION_TEMPLATE_RARE_DATA_TYPE",
  159: "INTERCEPTOR_INFO_TYPE",
  160: "INTERPRETER_DATA_TYPE",
  161: "MODULE_REQUEST_TYPE",
  162: "PROMISE_CAPABILITY_TYPE",
  163: "PROMISE_ON_STACK_TYPE",
  164: "PROMISE_REACTION_TYPE",
  165: "PROPERTY_DESCRIPTOR_OBJECT_TYPE",
  166: "PROTOTYPE_INFO_TYPE",
  167: "REG_EXP_BOILERPLATE_DESCRIPTION_TYPE",
  168: "SCRIPT_TYPE",
  169: "SCRIPT_OR_MODULE_TYPE",
  170: "SOURCE_TEXT_MODULE_INFO_ENTRY_TYPE",
  171: "STACK_FRAME_INFO_TYPE",
  172: "TEMPLATE_OBJECT_DESCRIPTION_TYPE",
  173: "TUPLE2_TYPE",
  174: "WASM_EXCEPTION_TAG_TYPE",
  175: "WASM_INDIRECT_FUNCTION_TABLE_TYPE",
  176: "FIXED_ARRAY_TYPE",
  177: "HASH_TABLE_TYPE",
  178: "EPHEMERON_HASH_TABLE_TYPE",
  179: "GLOBAL_DICTIONARY_TYPE",
  180: "NAME_DICTIONARY_TYPE",
  181: "NAME_TO_INDEX_HASH_TABLE_TYPE",
  182: "NUMBER_DICTIONARY_TYPE",
  183: "ORDERED_HASH_MAP_TYPE",
  184: "ORDERED_HASH_SET_TYPE",
  185: "ORDERED_NAME_DICTIONARY_TYPE",
  186: "REGISTERED_SYMBOL_TABLE_TYPE",
  187: "SIMPLE_NUMBER_DICTIONARY_TYPE",
  188: "CLOSURE_FEEDBACK_CELL_ARRAY_TYPE",
  189: "OBJECT_BOILERPLATE_DESCRIPTION_TYPE",
  190: "SCRIPT_CONTEXT_TABLE_TYPE",
  191: "BYTE_ARRAY_TYPE",
  192: "BYTECODE_ARRAY_TYPE",
  193: "FIXED_DOUBLE_ARRAY_TYPE",
  194: "INTERNAL_CLASS_WITH_SMI_ELEMENTS_TYPE",
  195: "SLOPPY_ARGUMENTS_ELEMENTS_TYPE",
  196: "TURBOFAN_BITSET_TYPE_TYPE",
  197: "TURBOFAN_HEAP_CONSTANT_TYPE_TYPE",
  198: "TURBOFAN_OTHER_NUMBER_CONSTANT_TYPE_TYPE",
  199: "TURBOFAN_RANGE_TYPE_TYPE",
  200: "TURBOFAN_UNION_TYPE_TYPE",
  201: "EXPORTED_SUB_CLASS_BASE_TYPE",
  202: "EXPORTED_SUB_CLASS_TYPE",
  203: "EXPORTED_SUB_CLASS2_TYPE",
  204: "FOREIGN_TYPE",
  205: "AWAIT_CONTEXT_TYPE",
  206: "BLOCK_CONTEXT_TYPE",
  207: "CATCH_CONTEXT_TYPE",
  208: "DEBUG_EVALUATE_CONTEXT_TYPE",
  209: "EVAL_CONTEXT_TYPE",
  210: "FUNCTION_CONTEXT_TYPE",
  211: "MODULE_CONTEXT_TYPE",
  212: "NATIVE_CONTEXT_TYPE",
  213: "SCRIPT_CONTEXT_TYPE",
  214: "WITH_CONTEXT_TYPE",
  215: "UNCOMPILED_DATA_WITH_PREPARSE_DATA_TYPE",
  216: "UNCOMPILED_DATA_WITH_PREPARSE_DATA_AND_JOB_TYPE",
  217: "UNCOMPILED_DATA_WITHOUT_PREPARSE_DATA_TYPE",
  218: "UNCOMPILED_DATA_WITHOUT_PREPARSE_DATA_WITH_JOB_TYPE",
  219: "WASM_FUNCTION_DATA_TYPE",
  220: "WASM_CAPI_FUNCTION_DATA_TYPE",
  221: "WASM_EXPORTED_FUNCTION_DATA_TYPE",
  222: "WASM_JS_FUNCTION_DATA_TYPE",
  223: "SMALL_ORDERED_HASH_MAP_TYPE",
  224: "SMALL_ORDERED_HASH_SET_TYPE",
  225: "SMALL_ORDERED_NAME_DICTIONARY_TYPE",
  226: "ABSTRACT_INTERNAL_CLASS_SUBCLASS1_TYPE",
  227: "ABSTRACT_INTERNAL_CLASS_SUBCLASS2_TYPE",
  228: "DESCRIPTOR_ARRAY_TYPE",
  229: "STRONG_DESCRIPTOR_ARRAY_TYPE",
  230: "SOURCE_TEXT_MODULE_TYPE",
  231: "SYNTHETIC_MODULE_TYPE",
  232: "WEAK_FIXED_ARRAY_TYPE",
  233: "TRANSITION_ARRAY_TYPE",
  234: "ACCESSOR_INFO_TYPE",
  235: "CALL_HANDLER_INFO_TYPE",
  236: "CELL_TYPE",
  237: "CODE_TYPE",
  238: "CODE_DATA_CONTAINER_TYPE",
  239: "COVERAGE_INFO_TYPE",
  240: "EMBEDDER_DATA_ARRAY_TYPE",
  241: "FEEDBACK_METADATA_TYPE",
  242: "FEEDBACK_VECTOR_TYPE",
  243: "FILLER_TYPE",
  244: "FREE_SPACE_TYPE",
  245: "INTERNAL_CLASS_TYPE",
  246: "INTERNAL_CLASS_WITH_STRUCT_ELEMENTS_TYPE",
  247: "MAP_TYPE",
  248: "MEGA_DOM_HANDLER_TYPE",
  249: "ON_HEAP_BASIC_BLOCK_PROFILER_DATA_TYPE",
  250: "PREPARSE_DATA_TYPE",
  251: "PROPERTY_ARRAY_TYPE",
  252: "PROPERTY_CELL_TYPE",
  253: "SCOPE_INFO_TYPE",
  254: "SHARED_FUNCTION_INFO_TYPE",
  255: "SMI_BOX_TYPE",
  256: "SMI_PAIR_TYPE",
  257: "SORT_STATE_TYPE",
  258: "SWISS_NAME_DICTIONARY_TYPE",
  259: "WASM_API_FUNCTION_REF_TYPE",
  260: "WASM_CONTINUATION_OBJECT_TYPE",
  261: "WASM_INTERNAL_FUNCTION_TYPE",
  262: "WASM_RESUME_DATA_TYPE",
  263: "WASM_STRING_VIEW_ITER_TYPE",
  264: "WASM_TYPE_INFO_TYPE",
  265: "WEAK_ARRAY_LIST_TYPE",
  266: "WEAK_CELL_TYPE",
  267: "WASM_ARRAY_TYPE",
  268: "WASM_STRUCT_TYPE",
  269: "JS_PROXY_TYPE",
  1057: "JS_OBJECT_TYPE",
  270: "JS_GLOBAL_OBJECT_TYPE",
  271: "JS_GLOBAL_PROXY_TYPE",
  272: "JS_MODULE_NAMESPACE_TYPE",
  1040: "JS_SPECIAL_API_OBJECT_TYPE",
  1041: "JS_PRIMITIVE_WRAPPER_TYPE",
  1058: "JS_API_OBJECT_TYPE",
  2058: "JS_LAST_DUMMY_API_OBJECT_TYPE",
  2059: "JS_DATA_VIEW_TYPE",
  2060: "JS_TYPED_ARRAY_TYPE",
  2061: "JS_ARRAY_BUFFER_TYPE",
  2062: "JS_PROMISE_TYPE",
  2063: "JS_BOUND_FUNCTION_TYPE",
  2064: "JS_WRAPPED_FUNCTION_TYPE",
  2065: "JS_FUNCTION_TYPE",
  2066: "BIGINT64_TYPED_ARRAY_CONSTRUCTOR_TYPE",
  2067: "BIGUINT64_TYPED_ARRAY_CONSTRUCTOR_TYPE",
  2068: "FLOAT32_TYPED_ARRAY_CONSTRUCTOR_TYPE",
  2069: "FLOAT64_TYPED_ARRAY_CONSTRUCTOR_TYPE",
  2070: "INT16_TYPED_ARRAY_CONSTRUCTOR_TYPE",
  2071: "INT32_TYPED_ARRAY_CONSTRUCTOR_TYPE",
  2072: "INT8_TYPED_ARRAY_CONSTRUCTOR_TYPE",
  2073: "UINT16_TYPED_ARRAY_CONSTRUCTOR_TYPE",
  2074: "UINT32_TYPED_ARRAY_CONSTRUCTOR_TYPE",
  2075: "UINT8_CLAMPED_TYPED_ARRAY_CONSTRUCTOR_TYPE",
  2076: "UINT8_TYPED_ARRAY_CONSTRUCTOR_TYPE",
  2077: "JS_ARRAY_CONSTRUCTOR_TYPE",
  2078: "JS_PROMISE_CONSTRUCTOR_TYPE",
  2079: "JS_REG_EXP_CONSTRUCTOR_TYPE",
  2080: "JS_CLASS_CONSTRUCTOR_TYPE",
  2081: "JS_ARRAY_ITERATOR_PROTOTYPE_TYPE",
  2082: "JS_ITERATOR_PROTOTYPE_TYPE",
  2083: "JS_MAP_ITERATOR_PROTOTYPE_TYPE",
  2084: "JS_OBJECT_PROTOTYPE_TYPE",
  2085: "JS_PROMISE_PROTOTYPE_TYPE",
  2086: "JS_REG_EXP_PROTOTYPE_TYPE",
  2087: "JS_SET_ITERATOR_PROTOTYPE_TYPE",
  2088: "JS_SET_PROTOTYPE_TYPE",
  2089: "JS_STRING_ITERATOR_PROTOTYPE_TYPE",
  2090: "JS_TYPED_ARRAY_PROTOTYPE_TYPE",
  2091: "JS_MAP_KEY_ITERATOR_TYPE",
  2092: "JS_MAP_KEY_VALUE_ITERATOR_TYPE",
  2093: "JS_MAP_VALUE_ITERATOR_TYPE",
  2094: "JS_SET_KEY_VALUE_ITERATOR_TYPE",
  2095: "JS_SET_VALUE_ITERATOR_TYPE",
  2096: "JS_GENERATOR_OBJECT_TYPE",
  2097: "JS_ASYNC_FUNCTION_OBJECT_TYPE",
  2098: "JS_ASYNC_GENERATOR_OBJECT_TYPE",
  2099: "JS_MAP_TYPE",
  2100: "JS_SET_TYPE",
  2101: "JS_ATOMICS_CONDITION_TYPE",
  2102: "JS_ATOMICS_MUTEX_TYPE",
  2103: "JS_WEAK_MAP_TYPE",
  2104: "JS_WEAK_SET_TYPE",
  2105: "JS_ARGUMENTS_OBJECT_TYPE",
  2106: "JS_ARRAY_TYPE",
  2107: "JS_ARRAY_ITERATOR_TYPE",
  2108: "JS_ASYNC_FROM_SYNC_ITERATOR_TYPE",
  2109: "JS_COLLATOR_TYPE",
  2110: "JS_CONTEXT_EXTENSION_OBJECT_TYPE",
  2111: "JS_DATE_TYPE",
  2112: "JS_DATE_TIME_FORMAT_TYPE",
  2113: "JS_DISPLAY_NAMES_TYPE",
  2114: "JS_ERROR_TYPE",
  2115: "JS_EXTERNAL_OBJECT_TYPE",
  2116: "JS_FINALIZATION_REGISTRY_TYPE",
  2117: "JS_LIST_FORMAT_TYPE",
  2118: "JS_LOCALE_TYPE",
  2119: "JS_MESSAGE_OBJECT_TYPE",
  2120: "JS_NUMBER_FORMAT_TYPE",
  2121: "JS_PLURAL_RULES_TYPE",
  2122: "JS_REG_EXP_TYPE",
  2123: "JS_REG_EXP_STRING_ITERATOR_TYPE",
  2124: "JS_RELATIVE_TIME_FORMAT_TYPE",
  2125: "JS_SEGMENT_ITERATOR_TYPE",
  2126: "JS_SEGMENTER_TYPE",
  2127: "JS_SEGMENTS_TYPE",
  2128: "JS_SHADOW_REALM_TYPE",
  2129: "JS_SHARED_ARRAY_TYPE",
  2130: "JS_SHARED_STRUCT_TYPE",
  2131: "JS_STRING_ITERATOR_TYPE",
  2132: "JS_TEMPORAL_CALENDAR_TYPE",
  2133: "JS_TEMPORAL_DURATION_TYPE",
  2134: "JS_TEMPORAL_INSTANT_TYPE",
  2135: "JS_TEMPORAL_PLAIN_DATE_TYPE",
  2136: "JS_TEMPORAL_PLAIN_DATE_TIME_TYPE",
  2137: "JS_TEMPORAL_PLAIN_MONTH_DAY_TYPE",
  2138: "JS_TEMPORAL_PLAIN_TIME_TYPE",
  2139: "JS_TEMPORAL_PLAIN_YEAR_MONTH_TYPE",
  2140: "JS_TEMPORAL_TIME_ZONE_TYPE",
  2141: "JS_TEMPORAL_ZONED_DATE_TIME_TYPE",
  2142: "JS_V8_BREAK_ITERATOR_TYPE",
  2143: "JS_WEAK_REF_TYPE",
  2144: "WASM_EXCEPTION_PACKAGE_TYPE",
  2145: "WASM_GLOBAL_OBJECT_TYPE",
  2146: "WASM_INSTANCE_OBJECT_TYPE",
  2147: "WASM_MEMORY_OBJECT_TYPE",
  2148: "WASM_MODULE_OBJECT_TYPE",
  2149: "WASM_SUSPENDER_OBJECT_TYPE",
  2150: "WASM_TABLE_OBJECT_TYPE",
  2151: "WASM_TAG_OBJECT_TYPE",
  2152: "WASM_VALUE_OBJECT_TYPE",
}

# List of known V8 maps.
KNOWN_MAPS = {
    ("read_only_space", 0x02139): (247, "MetaMap"),
    ("read_only_space", 0x02161): (131, "NullMap"),
    ("read_only_space", 0x02189): (229, "StrongDescriptorArrayMap"),
    ("read_only_space", 0x021b1): (265, "WeakArrayListMap"),
    ("read_only_space", 0x021f5): (155, "EnumCacheMap"),
    ("read_only_space", 0x02229): (176, "FixedArrayMap"),
    ("read_only_space", 0x02275): (8, "OneByteInternalizedStringMap"),
    ("read_only_space", 0x022c1): (244, "FreeSpaceMap"),
    ("read_only_space", 0x022e9): (243, "OnePointerFillerMap"),
    ("read_only_space", 0x02311): (243, "TwoPointerFillerMap"),
    ("read_only_space", 0x02339): (131, "UninitializedMap"),
    ("read_only_space", 0x023b1): (131, "UndefinedMap"),
    ("read_only_space", 0x023f5): (130, "HeapNumberMap"),
    ("read_only_space", 0x02429): (131, "TheHoleMap"),
    ("read_only_space", 0x02489): (131, "BooleanMap"),
    ("read_only_space", 0x0252d): (191, "ByteArrayMap"),
    ("read_only_space", 0x02555): (176, "FixedCOWArrayMap"),
    ("read_only_space", 0x0257d): (177, "HashTableMap"),
    ("read_only_space", 0x025a5): (128, "SymbolMap"),
    ("read_only_space", 0x025cd): (40, "OneByteStringMap"),
    ("read_only_space", 0x025f5): (253, "ScopeInfoMap"),
    ("read_only_space", 0x0261d): (254, "SharedFunctionInfoMap"),
    ("read_only_space", 0x02645): (237, "CodeMap"),
    ("read_only_space", 0x0266d): (236, "CellMap"),
    ("read_only_space", 0x02695): (252, "GlobalPropertyCellMap"),
    ("read_only_space", 0x026bd): (204, "ForeignMap"),
    ("read_only_space", 0x026e5): (233, "TransitionArrayMap"),
    ("read_only_space", 0x0270d): (45, "ThinOneByteStringMap"),
    ("read_only_space", 0x02735): (242, "FeedbackVectorMap"),
    ("read_only_space", 0x0276d): (131, "ArgumentsMarkerMap"),
    ("read_only_space", 0x027cd): (131, "ExceptionMap"),
    ("read_only_space", 0x02829): (131, "TerminationExceptionMap"),
    ("read_only_space", 0x02891): (131, "OptimizedOutMap"),
    ("read_only_space", 0x028f1): (131, "StaleRegisterMap"),
    ("read_only_space", 0x02951): (190, "ScriptContextTableMap"),
    ("read_only_space", 0x02979): (188, "ClosureFeedbackCellArrayMap"),
    ("read_only_space", 0x029a1): (241, "FeedbackMetadataArrayMap"),
    ("read_only_space", 0x029c9): (176, "ArrayListMap"),
    ("read_only_space", 0x029f1): (129, "BigIntMap"),
    ("read_only_space", 0x02a19): (189, "ObjectBoilerplateDescriptionMap"),
    ("read_only_space", 0x02a41): (192, "BytecodeArrayMap"),
    ("read_only_space", 0x02a69): (238, "CodeDataContainerMap"),
    ("read_only_space", 0x02a91): (239, "CoverageInfoMap"),
    ("read_only_space", 0x02ab9): (193, "FixedDoubleArrayMap"),
    ("read_only_space", 0x02ae1): (179, "GlobalDictionaryMap"),
    ("read_only_space", 0x02b09): (157, "ManyClosuresCellMap"),
    ("read_only_space", 0x02b31): (248, "MegaDomHandlerMap"),
    ("read_only_space", 0x02b59): (176, "ModuleInfoMap"),
    ("read_only_space", 0x02b81): (180, "NameDictionaryMap"),
    ("read_only_space", 0x02ba9): (157, "NoClosuresCellMap"),
    ("read_only_space", 0x02bd1): (182, "NumberDictionaryMap"),
    ("read_only_space", 0x02bf9): (157, "OneClosureCellMap"),
    ("read_only_space", 0x02c21): (183, "OrderedHashMapMap"),
    ("read_only_space", 0x02c49): (184, "OrderedHashSetMap"),
    ("read_only_space", 0x02c71): (181, "NameToIndexHashTableMap"),
    ("read_only_space", 0x02c99): (186, "RegisteredSymbolTableMap"),
    ("read_only_space", 0x02cc1): (185, "OrderedNameDictionaryMap"),
    ("read_only_space", 0x02ce9): (250, "PreparseDataMap"),
    ("read_only_space", 0x02d11): (251, "PropertyArrayMap"),
    ("read_only_space", 0x02d39): (234, "AccessorInfoMap"),
    ("read_only_space", 0x02d61): (235, "SideEffectCallHandlerInfoMap"),
    ("read_only_space", 0x02d89): (235, "SideEffectFreeCallHandlerInfoMap"),
    ("read_only_space", 0x02db1): (235, "NextCallSideEffectFreeCallHandlerInfoMap"),
    ("read_only_space", 0x02dd9): (187, "SimpleNumberDictionaryMap"),
    ("read_only_space", 0x02e01): (223, "SmallOrderedHashMapMap"),
    ("read_only_space", 0x02e29): (224, "SmallOrderedHashSetMap"),
    ("read_only_space", 0x02e51): (225, "SmallOrderedNameDictionaryMap"),
    ("read_only_space", 0x02e79): (230, "SourceTextModuleMap"),
    ("read_only_space", 0x02ea1): (258, "SwissNameDictionaryMap"),
    ("read_only_space", 0x02ec9): (231, "SyntheticModuleMap"),
    ("read_only_space", 0x02ef1): (259, "WasmApiFunctionRefMap"),
    ("read_only_space", 0x02f19): (220, "WasmCapiFunctionDataMap"),
    ("read_only_space", 0x02f41): (221, "WasmExportedFunctionDataMap"),
    ("read_only_space", 0x02f69): (261, "WasmInternalFunctionMap"),
    ("read_only_space", 0x02f91): (222, "WasmJSFunctionDataMap"),
    ("read_only_space", 0x02fb9): (262, "WasmResumeDataMap"),
    ("read_only_space", 0x02fe1): (264, "WasmTypeInfoMap"),
    ("read_only_space", 0x03009): (260, "WasmContinuationObjectMap"),
    ("read_only_space", 0x03031): (232, "WeakFixedArrayMap"),
    ("read_only_space", 0x03059): (178, "EphemeronHashTableMap"),
    ("read_only_space", 0x03081): (240, "EmbedderDataArrayMap"),
    ("read_only_space", 0x030a9): (266, "WeakCellMap"),
    ("read_only_space", 0x030d1): (32, "StringMap"),
    ("read_only_space", 0x030f9): (41, "ConsOneByteStringMap"),
    ("read_only_space", 0x03121): (33, "ConsStringMap"),
    ("read_only_space", 0x03149): (37, "ThinStringMap"),
    ("read_only_space", 0x03171): (35, "SlicedStringMap"),
    ("read_only_space", 0x03199): (43, "SlicedOneByteStringMap"),
    ("read_only_space", 0x031c1): (34, "ExternalStringMap"),
    ("read_only_space", 0x031e9): (42, "ExternalOneByteStringMap"),
    ("read_only_space", 0x03211): (50, "UncachedExternalStringMap"),
    ("read_only_space", 0x03239): (0, "InternalizedStringMap"),
    ("read_only_space", 0x03261): (2, "ExternalInternalizedStringMap"),
    ("read_only_space", 0x03289): (10, "ExternalOneByteInternalizedStringMap"),
    ("read_only_space", 0x032b1): (18, "UncachedExternalInternalizedStringMap"),
    ("read_only_space", 0x032d9): (26, "UncachedExternalOneByteInternalizedStringMap"),
    ("read_only_space", 0x03301): (58, "UncachedExternalOneByteStringMap"),
    ("read_only_space", 0x03329): (104, "SharedOneByteStringMap"),
    ("read_only_space", 0x03351): (96, "SharedStringMap"),
    ("read_only_space", 0x03379): (109, "SharedThinOneByteStringMap"),
    ("read_only_space", 0x033a1): (101, "SharedThinStringMap"),
    ("read_only_space", 0x033c9): (131, "SelfReferenceMarkerMap"),
    ("read_only_space", 0x033f1): (131, "BasicBlockCountersMarkerMap"),
    ("read_only_space", 0x03435): (146, "ArrayBoilerplateDescriptionMap"),
    ("read_only_space", 0x03535): (159, "InterceptorInfoMap"),
    ("read_only_space", 0x07359): (132, "PromiseFulfillReactionJobTaskMap"),
    ("read_only_space", 0x07381): (133, "PromiseRejectReactionJobTaskMap"),
    ("read_only_space", 0x073a9): (134, "CallableTaskMap"),
    ("read_only_space", 0x073d1): (135, "CallbackTaskMap"),
    ("read_only_space", 0x073f9): (136, "PromiseResolveThenableJobTaskMap"),
    ("read_only_space", 0x07421): (139, "FunctionTemplateInfoMap"),
    ("read_only_space", 0x07449): (140, "ObjectTemplateInfoMap"),
    ("read_only_space", 0x07471): (141, "AccessCheckInfoMap"),
    ("read_only_space", 0x07499): (142, "AccessorPairMap"),
    ("read_only_space", 0x074c1): (143, "AliasedArgumentsEntryMap"),
    ("read_only_space", 0x074e9): (144, "AllocationMementoMap"),
    ("read_only_space", 0x07511): (147, "AsmWasmDataMap"),
    ("read_only_space", 0x07539): (148, "AsyncGeneratorRequestMap"),
    ("read_only_space", 0x07561): (149, "BreakPointMap"),
    ("read_only_space", 0x07589): (150, "BreakPointInfoMap"),
    ("read_only_space", 0x075b1): (151, "CachedTemplateObjectMap"),
    ("read_only_space", 0x075d9): (152, "CallSiteInfoMap"),
    ("read_only_space", 0x07601): (153, "ClassPositionsMap"),
    ("read_only_space", 0x07629): (154, "DebugInfoMap"),
    ("read_only_space", 0x07651): (156, "ErrorStackDataMap"),
    ("read_only_space", 0x07679): (158, "FunctionTemplateRareDataMap"),
    ("read_only_space", 0x076a1): (160, "InterpreterDataMap"),
    ("read_only_space", 0x076c9): (161, "ModuleRequestMap"),
    ("read_only_space", 0x076f1): (162, "PromiseCapabilityMap"),
    ("read_only_space", 0x07719): (163, "PromiseOnStackMap"),
    ("read_only_space", 0x07741): (164, "PromiseReactionMap"),
    ("read_only_space", 0x07769): (165, "PropertyDescriptorObjectMap"),
    ("read_only_space", 0x07791): (166, "PrototypeInfoMap"),
    ("read_only_space", 0x077b9): (167, "RegExpBoilerplateDescriptionMap"),
    ("read_only_space", 0x077e1): (168, "ScriptMap"),
    ("read_only_space", 0x07809): (169, "ScriptOrModuleMap"),
    ("read_only_space", 0x07831): (170, "SourceTextModuleInfoEntryMap"),
    ("read_only_space", 0x07859): (171, "StackFrameInfoMap"),
    ("read_only_space", 0x07881): (172, "TemplateObjectDescriptionMap"),
    ("read_only_space", 0x078a9): (173, "Tuple2Map"),
    ("read_only_space", 0x078d1): (174, "WasmExceptionTagMap"),
    ("read_only_space", 0x078f9): (175, "WasmIndirectFunctionTableMap"),
    ("read_only_space", 0x07921): (195, "SloppyArgumentsElementsMap"),
    ("read_only_space", 0x07949): (228, "DescriptorArrayMap"),
    ("read_only_space", 0x07971): (217, "UncompiledDataWithoutPreparseDataMap"),
    ("read_only_space", 0x07999): (215, "UncompiledDataWithPreparseDataMap"),
    ("read_only_space", 0x079c1): (218, "UncompiledDataWithoutPreparseDataWithJobMap"),
    ("read_only_space", 0x079e9): (216, "UncompiledDataWithPreparseDataAndJobMap"),
    ("read_only_space", 0x07a11): (249, "OnHeapBasicBlockProfilerDataMap"),
    ("read_only_space", 0x07a39): (196, "TurbofanBitsetTypeMap"),
    ("read_only_space", 0x07a61): (200, "TurbofanUnionTypeMap"),
    ("read_only_space", 0x07a89): (199, "TurbofanRangeTypeMap"),
    ("read_only_space", 0x07ab1): (197, "TurbofanHeapConstantTypeMap"),
    ("read_only_space", 0x07ad9): (198, "TurbofanOtherNumberConstantTypeMap"),
    ("read_only_space", 0x07b01): (245, "InternalClassMap"),
    ("read_only_space", 0x07b29): (256, "SmiPairMap"),
    ("read_only_space", 0x07b51): (255, "SmiBoxMap"),
    ("read_only_space", 0x07b79): (201, "ExportedSubClassBaseMap"),
    ("read_only_space", 0x07ba1): (202, "ExportedSubClassMap"),
    ("read_only_space", 0x07bc9): (226, "AbstractInternalClassSubclass1Map"),
    ("read_only_space", 0x07bf1): (227, "AbstractInternalClassSubclass2Map"),
    ("read_only_space", 0x07c19): (194, "InternalClassWithSmiElementsMap"),
    ("read_only_space", 0x07c41): (246, "InternalClassWithStructElementsMap"),
    ("read_only_space", 0x07c69): (203, "ExportedSubClass2Map"),
    ("read_only_space", 0x07c91): (257, "SortStateMap"),
    ("read_only_space", 0x07cb9): (263, "WasmStringViewIterMap"),
    ("read_only_space", 0x07ce1): (145, "AllocationSiteWithWeakNextMap"),
    ("read_only_space", 0x07d09): (145, "AllocationSiteWithoutWeakNextMap"),
    ("read_only_space", 0x07dd5): (137, "LoadHandler1Map"),
    ("read_only_space", 0x07dfd): (137, "LoadHandler2Map"),
    ("read_only_space", 0x07e25): (137, "LoadHandler3Map"),
    ("read_only_space", 0x07e4d): (138, "StoreHandler0Map"),
    ("read_only_space", 0x07e75): (138, "StoreHandler1Map"),
    ("read_only_space", 0x07e9d): (138, "StoreHandler2Map"),
    ("read_only_space", 0x07ec5): (138, "StoreHandler3Map"),
    ("map_space", 0x02139): (2115, "ExternalMap"),
    ("map_space", 0x02161): (2119, "JSMessageObjectMap"),
}

# List of known V8 objects.
KNOWN_OBJECTS = {
  ("read_only_space", 0x021d9): "EmptyWeakArrayList",
  ("read_only_space", 0x021e5): "EmptyDescriptorArray",
  ("read_only_space", 0x0221d): "EmptyEnumCache",
  ("read_only_space", 0x02251): "EmptyFixedArray",
  ("read_only_space", 0x02259): "NullValue",
  ("read_only_space", 0x02361): "UninitializedValue",
  ("read_only_space", 0x023d9): "UndefinedValue",
  ("read_only_space", 0x0241d): "NanValue",
  ("read_only_space", 0x02451): "TheHoleValue",
  ("read_only_space", 0x0247d): "HoleNanValue",
  ("read_only_space", 0x024b1): "TrueValue",
  ("read_only_space", 0x024f1): "FalseValue",
  ("read_only_space", 0x02521): "empty_string",
  ("read_only_space", 0x0275d): "EmptyScopeInfo",
  ("read_only_space", 0x02795): "ArgumentsMarker",
  ("read_only_space", 0x027f5): "Exception",
  ("read_only_space", 0x02851): "TerminationException",
  ("read_only_space", 0x028b9): "OptimizedOut",
  ("read_only_space", 0x02919): "StaleRegister",
  ("read_only_space", 0x03419): "EmptyPropertyArray",
  ("read_only_space", 0x03421): "EmptyByteArray",
  ("read_only_space", 0x03429): "EmptyObjectBoilerplateDescription",
  ("read_only_space", 0x0345d): "EmptyArrayBoilerplateDescription",
  ("read_only_space", 0x03469): "EmptyClosureFeedbackCellArray",
  ("read_only_space", 0x03471): "EmptySlowElementDictionary",
  ("read_only_space", 0x03495): "EmptyOrderedHashMap",
  ("read_only_space", 0x034a9): "EmptyOrderedHashSet",
  ("read_only_space", 0x034bd): "EmptyFeedbackMetadata",
  ("read_only_space", 0x034c9): "EmptyPropertyDictionary",
  ("read_only_space", 0x034f1): "EmptyOrderedPropertyDictionary",
  ("read_only_space", 0x03509): "EmptySwissPropertyDictionary",
  ("read_only_space", 0x0355d): "NoOpInterceptorInfo",
  ("read_only_space", 0x03585): "EmptyArrayList",
  ("read_only_space", 0x03591): "EmptyWeakFixedArray",
  ("read_only_space", 0x03599): "InfinityValue",
  ("read_only_space", 0x035a5): "MinusZeroValue",
  ("read_only_space", 0x035b1): "MinusInfinityValue",
  ("read_only_space", 0x035bd): "SingleCharacterStringTable",
  ("read_only_space", 0x049c5): "SelfReferenceMarker",
  ("read_only_space", 0x04a05): "BasicBlockCountersMarker",
  ("read_only_space", 0x04a49): "OffHeapTrampolineRelocationInfo",
  ("read_only_space", 0x04a55): "GlobalThisBindingScopeInfo",
  ("read_only_space", 0x04a85): "EmptyFunctionScopeInfo",
  ("read_only_space", 0x04aa9): "NativeScopeInfo",
  ("read_only_space", 0x04ac1): "HashSeed",
  ("old_space", 0x04221): "ArgumentsIteratorAccessor",
  ("old_space", 0x04241): "ArrayLengthAccessor",
  ("old_space", 0x04261): "BoundFunctionLengthAccessor",
  ("old_space", 0x04281): "BoundFunctionNameAccessor",
  ("old_space", 0x042a1): "ErrorStackAccessor",
  ("old_space", 0x042c1): "FunctionArgumentsAccessor",
  ("old_space", 0x042e1): "FunctionCallerAccessor",
  ("old_space", 0x04301): "FunctionNameAccessor",
  ("old_space", 0x04321): "FunctionLengthAccessor",
  ("old_space", 0x04341): "FunctionPrototypeAccessor",
  ("old_space", 0x04361): "SharedArrayLengthAccessor",
  ("old_space", 0x04381): "StringLengthAccessor",
  ("old_space", 0x043a1): "ValueUnavailableAccessor",
  ("old_space", 0x043c1): "WrappedFunctionLengthAccessor",
  ("old_space", 0x043e1): "WrappedFunctionNameAccessor",
  ("old_space", 0x04401): "InvalidPrototypeValidityCell",
  ("old_space", 0x04409): "EmptyScript",
  ("old_space", 0x0444d): "ManyClosuresCell",
  ("old_space", 0x04459): "ArrayConstructorProtector",
  ("old_space", 0x0446d): "NoElementsProtector",
  ("old_space", 0x04481): "MegaDOMProtector",
  ("old_space", 0x04495): "IsConcatSpreadableProtector",
  ("old_space", 0x044a9): "ArraySpeciesProtector",
  ("old_space", 0x044bd): "TypedArraySpeciesProtector",
  ("old_space", 0x044d1): "PromiseSpeciesProtector",
  ("old_space", 0x044e5): "RegExpSpeciesProtector",
  ("old_space", 0x044f9): "StringLengthProtector",
  ("old_space", 0x0450d): "ArrayIteratorProtector",
  ("old_space", 0x04521): "ArrayBufferDetachingProtector",
  ("old_space", 0x04535): "PromiseHookProtector",
  ("old_space", 0x04549): "PromiseResolveProtector",
  ("old_space", 0x0455d): "MapIteratorProtector",
  ("old_space", 0x04571): "PromiseThenProtector",
  ("old_space", 0x04585): "SetIteratorProtector",
  ("old_space", 0x04599): "StringIteratorProtector",
  ("old_space", 0x045ad): "StringSplitCache",
  ("old_space", 0x049b5): "RegExpMultipleCache",
  ("old_space", 0x04dbd): "BuiltinsConstantsTable",
  ("old_space", 0x0520d): "AsyncFunctionAwaitRejectSharedFun",
  ("old_space", 0x05231): "AsyncFunctionAwaitResolveSharedFun",
  ("old_space", 0x05255): "AsyncGeneratorAwaitRejectSharedFun",
  ("old_space", 0x05279): "AsyncGeneratorAwaitResolveSharedFun",
  ("old_space", 0x0529d): "AsyncGeneratorYieldResolveSharedFun",
  ("old_space", 0x052c1): "AsyncGeneratorReturnResolveSharedFun",
  ("old_space", 0x052e5): "AsyncGeneratorReturnClosedRejectSharedFun",
  ("old_space", 0x05309): "AsyncGeneratorReturnClosedResolveSharedFun",
  ("old_space", 0x0532d): "AsyncIteratorValueUnwrapSharedFun",
  ("old_space", 0x05351): "PromiseAllResolveElementSharedFun",
  ("old_space", 0x05375): "PromiseAllSettledResolveElementSharedFun",
  ("old_space", 0x05399): "PromiseAllSettledRejectElementSharedFun",
  ("old_space", 0x053bd): "PromiseAnyRejectElementSharedFun",
  ("old_space", 0x053e1): "PromiseCapabilityDefaultRejectSharedFun",
  ("old_space", 0x05405): "PromiseCapabilityDefaultResolveSharedFun",
  ("old_space", 0x05429): "PromiseCatchFinallySharedFun",
  ("old_space", 0x0544d): "PromiseGetCapabilitiesExecutorSharedFun",
  ("old_space", 0x05471): "PromiseThenFinallySharedFun",
  ("old_space", 0x05495): "PromiseThrowerFinallySharedFun",
  ("old_space", 0x054b9): "PromiseValueThunkFinallySharedFun",
  ("old_space", 0x054dd): "ProxyRevokeSharedFun",
  ("old_space", 0x05501): "ShadowRealmImportValueFulfilledSFI",
  ("old_space", 0x05525): "SourceTextModuleExecuteAsyncModuleFulfilledSFI",
  ("old_space", 0x05549): "SourceTextModuleExecuteAsyncModuleRejectedSFI",
}

# Lower 32 bits of first page addresses for various heap spaces.
HEAP_FIRST_PAGES = {
  0x000c0000: "old_space",
  0x00100000: "map_space",
  0x00000000: "read_only_space",
}

# List of known V8 Frame Markers.
FRAME_MARKERS = (
  "ENTRY",
  "CONSTRUCT_ENTRY",
  "EXIT",
  "WASM",
  "WASM_TO_JS",
  "JS_TO_WASM",
  "STACK_SWITCH",
  "WASM_DEBUG_BREAK",
  "C_WASM_ENTRY",
  "WASM_EXIT",
  "WASM_COMPILE_LAZY",
  "INTERPRETED",
  "BASELINE",
  "MAGLEV",
  "TURBOFAN",
  "STUB",
  "BUILTIN_CONTINUATION",
  "JAVA_SCRIPT_BUILTIN_CONTINUATION",
  "JAVA_SCRIPT_BUILTIN_CONTINUATION_WITH_CATCH",
  "INTERNAL",
  "CONSTRUCT",
  "BUILTIN",
  "BUILTIN_EXIT",
  "NATIVE",
)

# This set of constants is generated from a shipping build.

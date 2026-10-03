# Upstream members with no field behind them

bg3le reads objects through field tables generated from bg3se's property maps,
and those tables only hold entries with a storage offset. Upstream also
declares members that are code: `P_FUN` (methods), `P_GETTER`,
`P_FREE_GETTER`, `P_GETTER_SETTER` and `P_FALLBACK` - 185 of them in
`GameDefinitions/Generated/PropertyMaps.inl`. Each exists in bg3le only if it
was written by hand, and no tool notices one that was not:
`tools/check-api.sh` covers `Ext` functions, and `tools/check-surface.sh`
covers the rest of `Ext` (tables such as `Ext.System`, values such as
`Ext.Config`'s), but neither reaches a member of an object.

Audited on 2026-10-03 against live objects in both contexts. Present: every
ImGui method and getter (`Text`, `DragDropType` and `ParentElement` added
that day), `TranslatedString:Get`, every `stats::Object` member, event
`StopPropagation`/`PreventAction`, `GameObjectTemplate.TemplateType` and
`TemplateStorageType`, `esv::Character:GetStatus`, and the deprecated
`ServerCharacter.Character` and `ServerItem.Item`.

Missing, none used by any of 75 installed mods when checked:

- Noesis objects: `TypeInfo`, `NumReferences`, `TreeParent`,
  `PointFromScreen`, `PointToScreen`, `HitTest`
- visuals: `SetWorldTranslate`, `SetWorldRotate`, `SetWorldScale`,
  `SetBlendShapeWeight`, `ClearBlendShapeWeights`, `BlendShape`; material
  `Get*`/`Set*` (not probed)
- `esv::Character:GetStatusByType` (the status type is a virtual call, not a
  field), `CreateCacheTemplate` on characters and items
- `ItemTemplate:AddUseAction`/`RemoveUseAction`
- `stats::Functors:AddNew`/`Remove`/`FunctorList` (probe inconclusive)
- system methods reached through `Ext.System`: `EffectsManager:Invoke` and
  `AddMaterial`, `AnimationBlueprintSystem:QueueGameplayEvent*`,
  `LevelInstanceAttachRequestSystem:RequestLevelSwap`,
  `ecl::effect::HandlerSystem`'s multi-effect methods
- rarely used getters, not probed: `esv::*State.Type`, `gn::GenomeVariant`,
  `aspk::Component.TypeName`, `ecs::ECSComponentLog`, `ecl::PlayerDragData`,
  `esv::surface::SurfaceComponent.Surface`, `StatsExpressionPooled`,
  `AiPath:UsePlayerWeighting`

To re-check, list the entries with
`grep -E "^(P_FUN|P_GETTER|P_FREE_GETTER|P_GETTER_SETTER|P_FALLBACK)\(" vendor/bg3se/BG3Extender/GameDefinitions/Generated/PropertyMaps.inl`
and index each on a live instance of its class.

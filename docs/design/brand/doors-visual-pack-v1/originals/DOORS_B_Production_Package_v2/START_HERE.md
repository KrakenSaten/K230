# DOORS B — production asset handoff v2

Forslag **B** er valgt: faste rammer, diskrete farger, tydelige appgrupper og fjellbildet som dempet bakgrunn.

Åpne `B_Portrait_overview.png`, `B_Landscape_overview.png` og `B_Icon_family.png`.
Gi deretter hele pakken og `PROJECT_HANDOFF_PROMPT.md` til prosjektets kodeassistent.

## Innhold

- 9 separate dørformede appikoner som redigerbar SVG og transparent PNG i 48, 64, 96 og 128 px. PNG-lerretene er kvadratiske, mens selve dørrammen er stående med transparente marger.
- 17 separate linjesymboler som SVG og 32 px PNG, for apper og systemkontroller.
- Gruppert launcher og systemmeny i portrett **568 × 1232** og liggende **1232 × 568**.
- Boot, låseskjerm og åpen dør fra forrige pakke i begge retninger.
- Totalt 10 rene native bakgrunner og 10 komplette native skjermvisninger.
- Separate redigerbare SVG-lag og transparente PNG-lag for UI.
- Plasseringer, farger og kontrollområder i JSON, font med lisens, integrasjonsnotat og overleveringsprompt.
- Det godkjente B-forslaget i `references/approved_B.png`.

## Bruk riktig fil

| Filtype | Bruk |
|---|---|
| `backgrounds/` | Produksjonsbakgrunner uten klokke, tekst eller kontroller |
| `icons/svg/` | Redigerbare masterikoner med farge og dørramme |
| `icons/png*/` | Transparente ikoner for UI-ressurser |
| `glyphs/` | Enkle symboler uten dørramme, til systemmenyen |
| `screens/` | Ferdige visuelle referanser med eksempeldata |
| `overlays/` | Redigerbare UI-referanser og transparente komposittlag |
| `b_ui_layout.json` | Launcher/systemmeny, farger, tekstposisjoner og kontrollområder |
| `base_ui_layout.json` | Bruk kun boot, lock og open; gammel launcher er erstattet av B |

Klokke, dato, status, fokus, brytere og skyveknapper skal tegnes dynamisk av prosjektet. Ikke bruk skjermbildene eller hele PNG-overlegg som produksjons-UI.

Ikonene og layoutene er presist gjenskapt som vektorer fra B-konseptet; de er ikke pikselutklipp av konseptbildet. Rammer og symboler er bevisst forenklet for liten skjerm. Nimbus Sans Regular brukes i referansene; vurder prosjektets eksisterende font ved integrasjon og kontroller bredder.

Dette er en visuell ressurs- og implementasjonspakke, ikke en ferdig firmwareendring. Native eksportmål, PNG-filer og kontrollområder er kontrollert. Faktisk K230-ytelse, AMOLED-farger og berøringsopplevelse må kontrolleres på enheten. Mellomframes til døranimasjon er ikke inkludert.

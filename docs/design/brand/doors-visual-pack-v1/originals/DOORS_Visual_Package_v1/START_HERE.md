# DOORS — visual package v1

Denne pakken inneholder boot, lock, open og launcher i begge orienteringer:

- Portrett: **568 × 1232 px**.
- Liggende: **1232 × 568 px**.

Åpne `Portrait_overview.png` og `Landscape_overview.png` for oversikten.
Gi hele pakken og `PROJECT_HANDOFF_PROMPT.md` til kodeassistenten som jobber med DOORS.

## Hva prosjektet skal bruke

| Mappe/fil | Innhold | Bruk |
|---|---|---|
| `backgrounds/` | 8 rene RGB PNG-bakgrunner i native oppløsning | Bakgrunn på enheten |
| `screens/` | 8 komplette native PNG-skjermvisninger | Visuell fasit; klokke og status er eksempler |
| `overlays/` | 8 redigerbare SVG-er og 8 transparente PNG-er | UI-referanse med separat tekst, klokke og status |
| `masters/` | 8 opprinnelige genererte bakgrunner i høyere oppløsning | Arkiv og eventuell ny eksport |
| `ui_layout.json` | Plasseringer, skriftstørrelser, farger og fokusfelt | Implementasjon av dynamisk UI |
| `fonts/` | Nimbus Sans Regular og lisensinformasjon | Reproduserbare forhåndsvisninger |
| `INTEGRATION.md` | Konkret integrasjonsbeskrivelse | Utvikling og kontroll på enheten |
| `PROJECT_HANDOFF_PROMPT.md` | Ferdig oppgave til kodeassistenten | Startpunkt for implementasjon |
| `tools/build_package.py` | Reproduserer native filer og UI fra masters | Valgfritt byggeverktøy |
| `asset_manifest.json` | Filstørrelser, dimensjoner og SHA-256 | Kontroll av leveransen |

## Avgrensning

Dette er en komplett **visuell overlevering for de fire skjermene**, ikke ferdig firmware eller et repository-patch. De rene bakgrunnene kan tas rett inn som PNG-ressurser. Skjermvisningene viser ønsket utseende, men klokke, dato, batteri, nettstatus, fremdrift og fokus skal rendres dynamisk.

Klokken 17:24, batteri 86 % og fremdrift 40 % er demonstrasjonsverdier. Applikasjonsnavn er kategorier fra designbriefen; bare installerbare/eksisterende mål skal kobles til virkelige handlinger.

Ingen appikoner er med. Statussymbolene er enkle separate UI-elementer. Appskjermer er ikke redesignet.

Bakgrunnene er laget med det innebygde bildeverktøyet, deretter eksportert til nøyaktig native størrelse. UI-lagene er laget som SVG, slik at teksten kan redigeres og gjenskapes i prosjektets UI-system. Nimbus Sans er brukt til forhåndsvisning; behold prosjektets eksisterende font dersom den fungerer, og kontroller tekstbreddene.

Åpen/lukket dør er separate stillbilder. Små geometriske forskjeller kan forekomme. Pakken inneholder **ikke** en ferdig registrert 6–8-bilders døranimasjon. Bruk statisk skifte i første integrasjon; bygg en egen verifisert animasjon senere dersom ønskelig.

PNG-dimensjoner og filintegritet er kontrollert. Ingen faktisk K230-kjøring eller AMOLED-kalibrering er utført.

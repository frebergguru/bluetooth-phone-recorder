# Accessibility / Tilgjengelighet

## English

Phone Recorder aims to be usable without a mouse, without sight and without
hearing every signal. This is the current state, checked on 2026-11-09:

**What works**

- Screen readers: every button, switch, slider and list has a name that says
  what it does and for which channel (for example "Send Host to the caller",
  "Volume for Guest"). Checked with AT-SPI: all 33 controls in the main window
  and the adapter chooser have names.
- Important events are announced to screen readers: incoming call (with the
  caller's name or number), call connected and ended, recording started and
  stopped, pairing requests with the code.
- Full keyboard control: Tab and Shift+Tab move between controls, Enter and
  Space activate them, Esc closes dialogs, focus is always visible. Shortcuts
  for everything important (F1 lists them): record Ctrl+R, marker Ctrl+M or
  F9, answer Ctrl+Shift+A, hang up Ctrl+Shift+H, input 1 to 9 heard by the
  caller Ctrl+1 to 9, mute Alt+1 to 9.
- Nothing depends on colour alone: the "In call" switch shows text and an
  icon, meters show the level as a number, clipping shows the word "CLIP".
- Nothing depends on sound alone: an incoming call is shown in the window, as
  a desktop notification and announced to screen readers.
- No time limits: nothing in the app has to be done within a set time. Only
  the phone's own pairing dialog times out (about 30 seconds), which the app
  cannot change.
- Text size: follows the desktop setting, plus the app's own setting from
  100 to 200 percent (Preferences, Accessibility, or Ctrl+plus and
  Ctrl+minus). It is saved. Text wraps instead of being cut off.
- Reduced motion: the app uses no animations of its own; libadwaita follows
  the system setting. Level meters move because they show live information.
- Contrast: uses the standard libadwaita colours, which meet WCAG AA, in
  light, dark and high contrast modes.
- Languages: English and Norwegian Bokmål.

**Known limits**

- Not yet tested with a real screen reader user, only with AT-SPI tools.
- The OBS source names are always in English so OBS scenes keep working.

Please report problems in writing.

## Norsk (bokmål)

Telefonopptak skal kunne brukes uten mus, uten syn og uten å høre alle
signaler. Dette er status per 9. november 2026:

**Dette virker**

- Skjermlesere: alle knapper, brytere, glidebrytere og lister har et navn som
  sier hva de gjør og for hvilken kanal (for eksempel "Send Vert til den som
  ringer", "Volum for Gjest"). Sjekket med AT-SPI: alle 33 kontrollene i
  hovedvinduet og adaptervalget har navn.
- Viktige hendelser leses opp av skjermleseren: innkommende anrop (med navn
  eller nummer), samtale koblet til og avsluttet, opptak startet og stoppet,
  paringsforespørsler med koden.
- Full tastaturstyring: Tab og Shift+Tab flytter mellom kontrollene, Enter og
  mellomrom aktiverer dem, Esc lukker dialoger, og fokus vises alltid.
  Hurtigtaster for alt viktig (F1 viser dem): opptak Ctrl+R, markør Ctrl+M
  eller F9, svar Ctrl+Shift+A, legg på Ctrl+Shift+H, inngang 1 til 9 hørbar
  for den som ringer Ctrl+1 til 9, demping Alt+1 til 9.
- Ingenting avhenger bare av farge: bryteren "I samtalen" viser tekst og et
  ikon, målerne viser nivået som tall, og klipping viser ordet "KLIPP".
- Ingenting avhenger bare av lyd: et innkommende anrop vises i vinduet, som
  et skrivebordsvarsel og leses opp av skjermleseren.
- Ingen tidsfrister: ingenting i appen må gjøres innen en bestemt tid. Bare
  telefonens egen paringsdialog går ut på tid (omtrent 30 sekunder), og det
  kan ikke appen endre.
- Tekststørrelse: følger skrivebordsinnstillingen, og i tillegg har appen sin
  egen innstilling fra 100 til 200 prosent (Innstillinger, Tilgjengelighet,
  eller Ctrl+pluss og Ctrl+minus). Den lagres. Tekst brytes i stedet for å
  bli kuttet.
- Redusert bevegelse: appen har ingen egne animasjoner; libadwaita følger
  systeminnstillingen. Nivåmålerne beveger seg fordi de viser levende
  informasjon.
- Kontrast: bruker standardfargene i libadwaita, som oppfyller WCAG AA, i
  lys, mørk og høy kontrast.
- Språk: engelsk og norsk bokmål.

**Kjente begrensninger**

- Ennå ikke testet av en som bruker skjermleser til daglig, bare med
  AT-SPI-verktøy.
- Navnene på OBS-kildene er alltid på engelsk, så OBS-scener fortsetter å
  virke.

Meld gjerne fra om problemer skriftlig.

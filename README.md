# hum2midi

Plugin VST3 para FL Studio: tarareas o cantas y tus notas aparecen como MIDI, en vivo y en tomas que puedes arrastrar al piano roll.

## Uso rápido

1. Carga hum2midi en la pista del mixer que tiene el micro como entrada. Usa auriculares.
2. Pulsa **Calibrar puerta** y quédate 1,5 s en silencio.
3. Pulsa **Grabar** y canta. Cada grabación queda como una toma en el panel de la derecha.
4. Escucha la toma con ▶ y arrástrala al piano roll de FL. También puedes usar **Guardar .mid**.

## Controles

**Detección**
- **Puerta**: el volumen mínimo para escuchar. **Calibrar puerta** la ajusta sola según el ruido de tu sala.
- **Detalle**: al 100 % capta adornos y temblores; más bajo se queda con la melodía principal.
- **Octava**: transporta la salida.

**Tonalidad**
- **Tónica / Escala**: las notas solo pueden caer dentro de la escala.
- **Detectar tonalidad**: la deduce de la toma seleccionada, o de lo último que has cantado, y la aplica.
- **Afinación auto**: aprende si cantas algo alto o bajo y lo compensa.

**Armonía y expresión**
- **Acordes**: tríadas, con séptima, quintas u octavas, de la escala elegida y por debajo de cada nota. Suenan en vivo y van en la toma exportada.
- **Pitch bend**: la rueda de pitch bend sigue tu voz. Ajusta el rango para que coincida con el del instrumento.
- **Velocity fija**: todas las notas a 100 en vez de según tu volumen.

**Monitor**
- **Oír synth / Volumen**: el synth interno, para oír lo que se detecta.
- **Tu voz**: mezcla tu voz en la salida para oírte.

**Tomas**
- **Grabar al dar Play**: la grabación arranca y para con el transporte de FL. La toma empieza en el compás donde arrancó, así que, si la arrastras a ese compás, cada nota cae donde la cantaste.
- **Rejilla / Fuerza / Legato**: la cuantización que se aplica al arrastrar o guardar. Se puede cambiar después de grabar.
- Se guardan hasta 12 tomas dentro del proyecto de FL.

## Compilar

GitHub Actions compila la versión de Windows en cada push (pestaña Actions → Artifacts). Para compilar en local: `cmake -B build && cmake --build build --config Release` (necesita Visual Studio con C++).

Tipografía: Barlow (SIL Open Font License, `Assets/OFL-Barlow.txt`).

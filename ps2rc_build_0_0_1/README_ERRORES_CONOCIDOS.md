# DBZ Budokai Tenkaichi 3 (SLUS-216.78) – PSP: Errores conocidos

> Based on PS2RC by SandstoneDev  
> https://github.com/SandstoneDev/PS2RC  
> Community: https://discord.gg/wV8x46ZTDu

Port por recompilación estática con PS2RC (SandstoneDev) y un renderizador propio para la GE de la PSP.
Probado en PPSSPP con la pelea de demostración (`demo_direct=1` en `ps2rc.cfg`). Todavía no se ha probado en una PSP real.

---

## Corregidos

### 1. Texturas basura en la pelea
Escenario y luchadores salían cubiertos de ruido de colores. Empezó después de las optimizaciones de velocidad.

**Solución:** los dibujos 3D nativos (programas VU1 convertidos a la GE) pedían memoria a la lista de comandos de la
GE sin comprobar si cabía. En la pelea, la lista se llenaba y los vértices se escribían encima del arena de texturas
en caché, que va justo detrás. Ahora, antes de cada petición, se comprueba el espacio libre y, si no cabe, se cierra
la lista y se empieza otra (`ge_getmem` en `gs_ge_vu1.h` y la misma comprobación en `ge_flush_batch` de `gs_ge.h`).

### 2. Uno de cada dos frames sale oscuro
Un frame sí y otro no, el escenario salía negro y los luchadores casi negros. Solo se veían el HUD y el polvo.

**Solución:** era la niebla por profundidad del juego, que funciona en dos pasadas:
1. Una pasada de 16 bits (FRAME CT16, FBMSK 0x3FFF) copia bits de profundidad al alfa del framebuffer.
2. Unos sprites restan color × alfa de destino (`ALPHA 0x52`: Cd − Cs·Ad).

La GE no puede reproducir la primera pasada, así que la resta usaba el alfa del propio 3D y oscurecía la imagen.
Ahora la segunda pasada no se dibuja (`ps2rc.cfg gs_dfog=1` la vuelve a activar). Además se saltan los sprites sin
textura con alfa 0 y mezcla (Cs−Cd)·As+Cd, que no cambian nada.

Otros cambios hechos durante esta corrección:
- El slot de destino se fija con `sceGuDrawBuffer`, para que un reinicio de lista no vuelva al slot 0.
- Los dos destinos se limpian al arrancar.
- Hay 32 KB de separación entre los buffers en VRAM.

### 3. Triángulo negro en el suelo (abajo a la izquierda)
Faltaba un trozo del suelo cerca de la cámara y en su lugar se veía una cuña negra.

**Solución:** la GE descarta el triángulo entero cuando algún vértice proyectado sale de su banda de guarda
(±2048 px), y los triángulos del suelo cercanos a la cámara salen de ella. En el programa 4 (escenario), esos
triángulos ahora se recortan en CPU por el plano cercano (w ≥ 1) y se subdividen en cuatro, hasta 3 niveles. Los
trozos que quedan del todo fuera de pantalla se descartan (`nat_draw3` en `gs_ge_vu1.h`; `ps2rc.cfg gs_clip=0` lo
desactiva).

### 4. Cuelgue al cargar la pelea
**Solución:** el microcódigo de la VU (`.vutext`) se lee del ELF original, porque en RAM lo pisaba el arena de texturas.

### 5. Parpadeo e imagen partida
**Solución:** se presenta una copia: la GE copia el destino terminado a uno de dos buffers de pantalla de 16 bits.

### 6. Neblina blanca y ruido en la demo 2 (Gotenks contra Buu) y en el modo Duelo
El escenario salía cubierto de ruido de colores y había una neblina blanca encima de todo.

**Solución:** el juego dibuja efectos en destinos pequeños de la memoria GS (0x150, 0x170) que la GE no dibuja, y
después los usa como textura de 32 bits. Se convertían desde la memoria GS sin ese dibujo: datos viejos, de ahí el
ruido y la neblina. Ahora las páginas de un destino no dibujado (solo las filas de su scissor) se marcan como
"renderizadas" y las texturas leídas de ellas se saltan (`gs_mark_skipped=0` lo desactiva). Además:
- la lista GE, el arena de texturas y el buffer VIF se reservan con `sceKernelAllocPartitionMemory`, fuera de la zona
  de código del juego;
- se emula el buffer de CLUT del GS (cargas según el campo CLD de TEX0/TEX2; `gs_cld=0` lo desactiva);
- la textura en uso queda protegida tras un reinicio de lista;
- la "row hint" se reinicia en cada lote 3D.

---

## Pendientes

### Cielo oscuro en la demo 2
El cielo azul con nubes sale gris oscuro.

**Solución prevista:** el juego lo compone desde una textura que renderiza él mismo (un destino CT16 en 0x150). Hace
falta renderizar a textura con la GE (dibujar esos destinos en VRAM y usarlos como textura).

### 6. Contorno de los personajes roto
El borde negro de los luchadores sale cortado o desplazado.

**Solución prevista:** una opción `gs_outline=0` que salte `ObjOutline_Draw` (0x105CD8) para quitar el contorno, como
en el port de PC. Arreglarlo de verdad requiere reproducir la pasada que el juego dibuja sobre la página de
profundidad.

### 7. Omega demasiado blanco
El cel-shading del personaje sale casi blanco.

**Solución prevista:** revisar la capa 1 del sombreado (mezcla REVERSE_SUBTRACT) en el programa 0 y su traducción a
`sceGuBlendFunc`.

### 8. Sin niebla por profundidad
Con la corrección 2, el escenario lejano ya no se oscurece con la distancia como en PS2.

**Solución prevista:** reproducir la profundidad→alfa con la GE: un render del Z a textura o una niebla por vértice
(`sceGuFog`) en los programas nativos.

### 9. Velocidad
En PPSSPP, la pelea va a ~0.14× y la pantalla de título a ~0.65× de la velocidad real.

**Solución prevista:** más programas VU1 nativos (el programa 6, `0x5EDC8A7C`, sigue interpretado) y menos
conversiones de texturas.

---

## Opciones de depuración (`ps2rc.cfg`)
| Opción | Efecto |
|---|---|
| `demo_direct=1` | Arranca directamente en la pelea de demostración |
| `gs_burst=N` / `gs_burstf=N` / `gs_burst_at=V` | Capturas de pantalla: 3 cada N segundos / 1 cada N vblanks / a partir del vblank V |
| `gs_dfog=1` | Vuelve a dibujar la pasada de niebla (reproduce el error 2) |
| `gs_clip=0` | Sin recorte en CPU (reproduce el error 3) |
| `gs_verify=1` | Comprueba que la caché de texturas coincide con la memoria GS |
| `gs_native=0` | VU1 interpretada en vez de nativa (muy lento) |

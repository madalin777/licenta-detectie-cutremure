# Sistem IoT pentru Detecția Cutremurelor

Sistem IoT pentru detecția în timp real a cutremurelor, cu alertare rapidă prin aplicație mobilă, dashboard web și notificări push. Proiect realizat ca lucrare de licență la Facultatea de Inginerie Electrică și Știința Calculatoarelor, Universitatea Transilvania din Brașov.

## Descriere

Sistemul monitorizează continuu vibrațiile solului cu un accelerometru, iar la depășirea unui prag de accelerație declanșează o alarmă locală (buzzer + LCD), înregistrează evenimentul pe card SD și trimite o notificare push către aplicația mobilă. Poziția geografică a evenimentului este determinată prin GPS și afișată pe hartă.

## Componente hardware

| Componentă | Rol | Protocol |
|---|---|---|
| ESP32 NodeMCU-32S | Microcontroller principal | — |
| MPU6050 | Accelerometru pe 3 axe (detecție vibrații) | I2C |
| GPS NEO-7M | Poziționare geografică | UART |
| LCD 16x2 cu I2C | Afișare stare și alerte | I2C |
| Modul card MicroSD | Salvarea locală a datelor (CSV) | SPI |
| Buzzer activ | Alarmă sonoră | — |

## Software

- **Firmware** (C++, PlatformIO / VS Code) — citirea senzorilor, filtrarea semnalului, logica de detecție, alarmă, logare pe SD și server web integrat.
- **Dashboard web** (HTML + Chart.js + Leaflet) — vizualizare live a accelerației și a evenimentelor pe grafic și hartă, cu buton de descărcare a datelor.
- **Aplicație mobilă** (React Native / Expo) — notificări push, istoric persistent al alertelor și afișarea locației pe hartă.

## Cum funcționează detecția

1. Accelerometrul este citit periodic (la 50 ms), iar valorile brute sunt convertite în G.
2. Cele trei axe sunt combinate într-o singură valoare a accelerației totale.
3. Se scade valoarea de repaus (baseline) determinată la calibrare, pentru a elimina gravitația și a păstra doar vibrația reală.
4. Semnalul este netezit printr-un filtru de medie mobilă (5 eșantioane), pentru reducerea zgomotului.
5. Sub pragul de zgomot (**0,03 G**) valoarea este considerată zero.
6. La atingerea sau depășirea pragului de seism (**0,10 G**) sistemul declanșează alarma și logează evenimentul.

Evenimentele sunt clasificate automat în funcție de intensitate: **Slab**, **Moderat** sau **Puternic**.

## Structura datelor salvate (CSV)

Fișierul `seism_complet.csv` conține, pentru fiecare eveniment: număr de ordine, dată, oră, accelerație totală (G), valorile pe axele X / Y / Z, coordonatele GPS (latitudine, longitudine), sursa poziției și clasificarea intensității.

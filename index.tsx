import React, { useState, useEffect, useRef, useCallback } from 'react';
import {
  Text,
  View,
  StyleSheet,
  TouchableOpacity,
  Platform,
  Share,
  FlatList,
  Vibration,
  Animated,
  Linking,
} from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import * as Device from 'expo-device';
import * as Notifications from 'expo-notifications';

// ==============================================================
// CONFIGURARE NOTIFICĂRI
// ==============================================================
Notifications.setNotificationHandler({
  handleNotification: async () => ({
    shouldShowAlert: true,
    shouldPlaySound: true,
    shouldSetBadge: false,
  }),
});

// ==============================================================
// CHEI ASYNCSTORAGE
// ==============================================================
const STORAGE_KEY_HISTORY = '@seism_history';
const MAX_HISTORY_ITEMS = 50; // Păstrăm ultimele 50 de alerte

export default function App() {
  const [expoPushToken, setExpoPushToken] = useState('');
  const [tokenError, setTokenError] = useState(false);
  const [history, setHistory] = useState([]);
  const [isAlarmActive, setIsAlarmActive] = useState(false);
  const notificationListener = useRef();
  const alarmOpacity = useRef(new Animated.Value(0)).current;
  const alarmTimeout = useRef(null);

  // --- ANIMAȚIE OVERLAY ROȘU PULSANT ---
  const startAlarmAnimation = () => {
    setIsAlarmActive(true);
    Animated.loop(
      Animated.sequence([
        Animated.timing(alarmOpacity, {
          toValue: 0.4,
          duration: 300,
          useNativeDriver: true,
        }),
        Animated.timing(alarmOpacity, {
          toValue: 0.1,
          duration: 300,
          useNativeDriver: true,
        }),
      ])
    ).start();
  };

  const stopAlarmAnimation = () => {
    alarmOpacity.stopAnimation();
    Animated.timing(alarmOpacity, {
      toValue: 0,
      duration: 500,
      useNativeDriver: true,
    }).start(() => setIsAlarmActive(false));
  };

  // --- VIBRAȚIE ALARMĂ (4 secunde) ---
  const triggerAlarm = () => {
    Vibration.vibrate([500, 500, 500, 500, 500, 500, 500, 500]);
    startAlarmAnimation();

    // Oprește animația după 5 secunde
    if (alarmTimeout.current) clearTimeout(alarmTimeout.current);
    alarmTimeout.current = setTimeout(() => {
      stopAlarmAnimation();
    }, 5000);
  };

  // --- PERSISTENȚĂ: Încarcă istoricul din AsyncStorage ---
  const loadHistory = async () => {
    try {
      const stored = await AsyncStorage.getItem(STORAGE_KEY_HISTORY);
      if (stored) {
        setHistory(JSON.parse(stored));
      }
    } catch (e) {
      console.warn('Eroare la încărcarea istoricului:', e);
    }
  };

  // --- PERSISTENȚĂ: Salvează istoricul în AsyncStorage ---
  const saveHistory = async (newHistory) => {
    try {
      // Limităm la MAX_HISTORY_ITEMS
      const trimmed = newHistory.slice(0, MAX_HISTORY_ITEMS);
      await AsyncStorage.setItem(STORAGE_KEY_HISTORY, JSON.stringify(trimmed));
    } catch (e) {
      console.warn('Eroare la salvarea istoricului:', e);
    }
  };

  // --- GOLIRE ISTORIC ---
  const clearHistory = async () => {
    setHistory([]);
    await AsyncStorage.removeItem(STORAGE_KEY_HISTORY);
  };

  // --- REVERSE GEOCODING: Convertim coordonate GPS în adresă reală ---
  // Folosim API-ul gratuit Nominatim (OpenStreetMap)
  const reverseGeocode = async (lat, lng) => {
    try {
      const response = await fetch(
        `https://nominatim.openstreetmap.org/reverse?format=json&lat=${lat}&lon=${lng}&zoom=18&addressdetails=1`,
        { headers: { 'Accept-Language': 'ro' } } // Răspuns în română
      );
      const data = await response.json();
      if (data && data.address) {
        const addr = data.address;
        // Construim adresa cât mai scurtă și utilă
        const parts = [];
        if (addr.road) parts.push(addr.road);
        if (addr.house_number) parts.push('nr. ' + addr.house_number);
        if (addr.city || addr.town || addr.village) {
          parts.push(addr.city || addr.town || addr.village);
        }
        return parts.join(', ') || data.display_name;
      }
      return null;
    } catch (e) {
      console.warn('Reverse geocoding eșuat:', e);
      return null;
    }
  };

  useEffect(() => {
    // Încarcă istoricul persistent la pornire
    loadHistory();

    registerForPushNotificationsAsync().then(result => {
      if (result.error) {
        setTokenError(true);
        setExpoPushToken(result.token);
      } else {
        setExpoPushToken(result.token);
      }
    });

    notificationListener.current = Notifications.addNotificationReceivedListener(async (notification) => {
      const { title, body, data } = notification.request.content;

      const now = new Date();
      const timeStr =
        now.getHours().toString().padStart(2, '0') + ':' +
        now.getMinutes().toString().padStart(2, '0') + ':' +
        now.getSeconds().toString().padStart(2, '0');

      const dateStr =
        now.getDate().toString().padStart(2, '0') + '/' +
        (now.getMonth() + 1).toString().padStart(2, '0') + '/' +
        now.getFullYear();

      // Reverse geocoding — convertim coordonatele în adresă
      let adresaReala = null;
      const lat = data?.lat;
      const lng = data?.lng;
      const gpsValid = data?.gpsValid === 'true';

      if (gpsValid && lat && lng && lat !== '0.000000' && lng !== '0.000000') {
        adresaReala = await reverseGeocode(lat, lng);
      }

      // Construim body-ul cu adresa reală
      let bodyComplet = body;
      if (adresaReala) {
        bodyComplet = body + '\n📍 ' + adresaReala;
      } else if (!gpsValid) {
        bodyComplet = body + '\n📍 GPS indisponibil';
      }

      const newEntry = {
        id: Date.now().toString(),
        title,
        body: bodyComplet,
        time: timeStr,
        date: dateStr,
        lat: lat || null,
        lng: lng || null,
        address: adresaReala,
      };

      setHistory(prevHistory => {
        const updated = [newEntry, ...prevHistory].slice(0, MAX_HISTORY_ITEMS);
        saveHistory(updated);
        return updated;
      });

      // DECLANȘĂM ALARMA VIZUALĂ + HAPTICĂ
      triggerAlarm();
    });

    return () => {
      Notifications.removeNotificationSubscription(notificationListener.current);
      if (alarmTimeout.current) clearTimeout(alarmTimeout.current);
    };
  }, []);

  const shareToken = async () => {
    try {
      await Share.share({ message: expoPushToken });
    } catch (error) {
      alert(error.message);
    }
  };

  // --- Deschide locația în Google Maps ---
  const openInMaps = (lat, lng) => {
    if (lat && lng && lat !== '0.000000' && lng !== '0.000000') {
      const url = `https://www.google.com/maps?q=${lat},${lng}`;
      Linking.openURL(url);
    }
  };

  // --- RENDERARE ELEMENT ISTORIC (FlatList) ---
  const renderHistoryItem = useCallback(({ item }) => (
    <View style={styles.historyItem}>
      <View style={styles.itemHeader}>
        <View>
          <Text style={styles.itemTime}>{item.time}</Text>
          <Text style={styles.itemDate}>{item.date}</Text>
        </View>
        <Text style={styles.itemType}>⚠️ SEISM</Text>
      </View>
      <Text style={styles.itemBody}>{item.body}</Text>
      {item.address && (
        <Text style={styles.itemAddress}>📍 {item.address}</Text>
      )}
      {item.lat && item.lng && item.lat !== '0.000000' && (
        <TouchableOpacity
          style={styles.mapButton}
          onPress={() => openInMaps(item.lat, item.lng)}
        >
          <Text style={styles.mapButtonText}>🗺️ DESCHIDE ÎN MAPS</Text>
        </TouchableOpacity>
      )}
    </View>
  ), []);

  const keyExtractor = useCallback((item) => item.id, []);

  return (
    <View style={styles.container}>
      {/* OVERLAY ROȘU PULSANT ÎN CAZ DE ALARMĂ */}
      {isAlarmActive && (
        <Animated.View
          style={[
            styles.alarmOverlay,
            { opacity: alarmOpacity },
          ]}
          pointerEvents="none"
        />
      )}

      <View style={styles.header}>
        <Text style={styles.title}>📡 DETECTOR SEISM</Text>
        <Text style={styles.subtitle}>Monitorizare Alerte în Timp Real</Text>
      </View>

      <View style={styles.card}>
        <Text style={styles.cardLabel}>ID DISPOZITIV:</Text>
        <Text style={[
          styles.tokenText,
          tokenError && styles.tokenTextError
        ]}>
          {expoPushToken || 'Se generează...'}
        </Text>
        {tokenError && (
          <Text style={styles.errorHint}>
            ⚠ Token-ul nu a putut fi generat. Verifică conexiunea.
          </Text>
        )}
        <TouchableOpacity style={styles.button} onPress={shareToken}>
          <Text style={styles.buttonText}>TRIMITE TOKEN</Text>
        </TouchableOpacity>
      </View>

      <View style={styles.historyContainer}>
        <View style={styles.historyHeader}>
          <Text style={styles.historyTitle}>📜 ISTORIC ALERTE:</Text>
          {history.length > 0 && (
            <TouchableOpacity onPress={clearHistory}>
              <Text style={styles.clearText}>ȘTERGE TOT</Text>
            </TouchableOpacity>
          )}
        </View>

        {history.length === 0 ? (
          <Text style={styles.emptyText}>Nicio alertă detectată încă.</Text>
        ) : (
          <FlatList
            data={history}
            renderItem={renderHistoryItem}
            keyExtractor={keyExtractor}
            style={styles.flatList}
            showsVerticalScrollIndicator={false}
            initialNumToRender={10}
            maxToRenderPerBatch={10}
            windowSize={5}
          />
        )}
      </View>
    </View>
  );
}

// ==============================================================
// ÎNREGISTRARE PUSH NOTIFICATIONS
// ==============================================================
async function registerForPushNotificationsAsync() {
  let token;
  let error = false;

  if (Device.isDevice) {
    const { status: existingStatus } = await Notifications.getPermissionsAsync();
    let finalStatus = existingStatus;
    if (existingStatus !== 'granted') {
      const { status } = await Notifications.requestPermissionsAsync();
      finalStatus = status;
    }
    if (finalStatus !== 'granted') {
      return { token: 'Permisiuni respinse', error: true };
    }
    try {
      token = (await Notifications.getExpoPushTokenAsync({
        projectId: '0542e7e7-d268-43f7-99b7-7a8139799eab',
      })).data;
    } catch (e) {
      console.error('Eroare la obținerea token-ului:', e);
      // NU mai returnăm un token hardcodat — raportăm eroarea
      return { token: 'Eroare: ' + e.message, error: true };
    }
  } else {
    return { token: 'Simulatoarele nu suportă push', error: true };
  }

  if (Platform.OS === 'android') {
    await Notifications.setNotificationChannelAsync('default', {
      name: 'Alerte Seism',
      importance: Notifications.AndroidImportance.MAX,
      vibrationPattern: [0, 250, 250, 250],
      lightColor: '#FF231F7C',
      sound: 'default',
    });
  }

  return { token, error: false };
}

// ==============================================================
// STILURI
// ==============================================================
const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#000',
    paddingTop: 60,
    paddingHorizontal: 20,
  },
  // OVERLAY ROȘU PULSANT
  alarmOverlay: {
    ...StyleSheet.absoluteFillObject,
    backgroundColor: '#FF3B30',
    zIndex: 999,
  },
  header: {
    alignItems: 'center',
    marginBottom: 25,
  },
  title: {
    fontSize: 24,
    fontWeight: 'bold',
    color: '#FF3B30',
  },
  subtitle: {
    color: '#8E8E93',
    fontSize: 12,
    marginTop: 4,
  },
  card: {
    backgroundColor: '#1C1C1E',
    padding: 20,
    borderRadius: 15,
    alignItems: 'center',
  },
  cardLabel: {
    color: '#8E8E93',
    fontSize: 10,
    marginBottom: 10,
  },
  tokenText: {
    color: '#0A84FF',
    fontSize: 11,
    marginBottom: 10,
    textAlign: 'center',
  },
  tokenTextError: {
    color: '#FF9500',
  },
  errorHint: {
    color: '#FF9500',
    fontSize: 10,
    marginBottom: 10,
    textAlign: 'center',
  },
  button: {
    backgroundColor: '#34C759',
    paddingVertical: 10,
    paddingHorizontal: 20,
    borderRadius: 8,
  },
  buttonText: {
    color: '#FFF',
    fontWeight: 'bold',
  },
  historyContainer: {
    flex: 1,
    marginTop: 25,
  },
  historyHeader: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    marginBottom: 10,
  },
  historyTitle: {
    color: '#FFF',
    fontSize: 14,
    fontWeight: 'bold',
  },
  clearText: {
    color: '#FF453A',
    fontSize: 11,
    fontWeight: '600',
  },
  flatList: {
    flex: 1,
  },
  emptyText: {
    color: '#48484A',
    textAlign: 'center',
    marginTop: 20,
  },
  historyItem: {
    backgroundColor: '#1C1C1E',
    padding: 15,
    borderRadius: 10,
    marginBottom: 10,
    borderLeftWidth: 4,
    borderLeftColor: '#FF3B30',
  },
  itemHeader: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'flex-start',
    marginBottom: 5,
  },
  itemTime: {
    color: '#EBEBF5',
    fontSize: 13,
    fontWeight: '600',
  },
  itemDate: {
    color: '#636366',
    fontSize: 10,
    marginTop: 2,
  },
  itemType: {
    color: '#FF3B30',
    fontSize: 11,
    fontWeight: 'bold',
  },
  itemBody: {
    color: '#EBEBF5',
    fontSize: 14,
    marginBottom: 8,
  },
  itemAddress: {
    color: '#34C759',
    fontSize: 13,
    fontWeight: '600',
    marginBottom: 8,
  },
  mapButton: {
    backgroundColor: 'rgba(10, 132, 255, 0.15)',
    paddingVertical: 6,
    paddingHorizontal: 12,
    borderRadius: 6,
    alignSelf: 'flex-start',
  },
  mapButtonText: {
    color: '#0A84FF',
    fontSize: 11,
    fontWeight: '600',
  },
});

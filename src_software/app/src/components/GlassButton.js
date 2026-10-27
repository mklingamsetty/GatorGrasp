import React from 'react';
import { Pressable, Text, StyleSheet } from 'react-native';

// Reusable "liquid glass-ish" button.
// variant: 'glass' (default, frosted), 'primary' (UF orange glow), 'danger' (soft red)
export default function GlassButton({ title, onPress, variant = 'glass', style }) {
  return (
    <Pressable
      onPress={onPress}
      style={({ pressed }) => [
        styles.base,
        variants[variant],
        pressed && styles.pressed,
        style,
      ]}
    >
      <Text style={styles.text}>{title}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  base: {
    borderRadius: 28,
    borderWidth: 1,
    paddingVertical: 15,
    paddingHorizontal: 22,
    marginVertical: 6,
    alignItems: 'center',
    shadowColor: '#000',
    shadowOffset: { width: 0, height: 8 },
    shadowOpacity: 0.25,
    shadowRadius: 16,
    elevation: 6,
  },
  pressed: {
    opacity: 0.75,
    transform: [{ scale: 0.97 }],
  },
  text: {
    color: '#fff',
    fontSize: 16,
    fontWeight: '600',
    letterSpacing: 0.3,
  },
});

const variants = StyleSheet.create({
  glass: {
    backgroundColor: 'rgba(255,255,255,0.12)',
    borderColor: 'rgba(255,255,255,0.28)',
  },
  primary: {
    backgroundColor: 'rgba(250,70,22,0.55)',
    borderColor: 'rgba(255,170,140,0.6)',
    shadowColor: '#FA4616',
    shadowOpacity: 0.5,
  },
  danger: {
    backgroundColor: 'rgba(255,80,80,0.18)',
    borderColor: 'rgba(255,120,120,0.4)',
  },
});

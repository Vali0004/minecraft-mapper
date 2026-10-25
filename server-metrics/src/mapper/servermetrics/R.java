// SPDX-License-Identifier: MPL-2.0
package mapper.servermetrics;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

// reflection by srg name, falling back to the deobfuscated one for dev environments. lookups are cached,
// misses included (as MISSING), anything missing is null rather than an exception
final class R {
  static final Object MISSING = new Object();
  static final Map<String, Object> fields = new ConcurrentHashMap<>();
  static final Map<String, Object> methods = new ConcurrentHashMap<>();
  static final Map<String, Object> classes = new ConcurrentHashMap<>();

  static Class<?> cls(String name) {
    Object c = classes.computeIfAbsent(name, n -> {
      try { return Class.forName(n); } catch (Throwable t) { return MISSING; }
    });
    return c == MISSING ? null : (Class<?>) c;
  }

  static boolean is(Object o, String className) {
    Class<?> c = cls(className);
    return o != null && c != null && c.isInstance(o);
  }

  static Field field(Class<?> c, String srg, String mcp) {
    Object f = fields.computeIfAbsent(c.getName() + "." + srg, key -> {
      for (Class<?> k = c; k != null; k = k.getSuperclass()) {
        for (String n : new String[] { srg, mcp }) {
          try {
            Field found = k.getDeclaredField(n);
            found.setAccessible(true);
            return found;
          } catch (NoSuchFieldException | SecurityException e) {}
        }
      }
      return MISSING;
    });
    return f == MISSING ? null : (Field) f;
  }

  static Object get(Object o, String srg, String mcp) {
    if (o == null) return null;
    Field f = field(o.getClass(), srg, mcp);
    try { return f == null ? null : f.get(o); } catch (Throwable t) { return null; }
  }

  static Object getStatic(String className, String srg, String mcp) {
    Class<?> c = cls(className);
    Field f = c == null ? null : field(c, srg, mcp);
    try { return f == null ? null : f.get(null); } catch (Throwable t) { return null; }
  }

  static Method method(Class<?> c, String srg, String mcp, Class<?>... args) {
    StringBuilder key = new StringBuilder(c.getName()).append('.').append(srg);
    for (Class<?> a : args) key.append(',').append(a.getName());
    Object m = methods.computeIfAbsent(key.toString(), k0 -> {
      for (Class<?> k = c; k != null; k = k.getSuperclass()) {
        for (String n : new String[] { srg, mcp }) {
          try {
            Method found = k.getDeclaredMethod(n, args);
            found.setAccessible(true);
            return found;
          } catch (NoSuchMethodException | SecurityException e) {}
        }
      }
      return MISSING;
    });
    return m == MISSING ? null : (Method) m;
  }

  static Object call(Object o, String srg, String mcp) {
    if (o == null) return null;
    Method m = method(o.getClass(), srg, mcp);
    try { return m == null ? null : m.invoke(o); } catch (Throwable t) { return null; }
  }

  static Object callStatic(String className, String srg, String mcp) {
    Class<?> c = cls(className);
    Method m = c == null ? null : method(c, srg, mcp);
    try { return m == null ? null : m.invoke(null); } catch (Throwable t) { return null; }
  }

  static Object callStatic1(String className, String srg, String mcp, String argClass, Object arg) {
    Class<?> c = cls(className), a = cls(argClass);
    Method m = c == null || a == null ? null : method(c, srg, mcp, a);
    try { return m == null ? null : m.invoke(null, arg); } catch (Throwable t) { return null; }
  }
}

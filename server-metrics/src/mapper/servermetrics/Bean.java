// SPDX-License-Identifier: MPL-2.0
package mapper.servermetrics;

import java.util.Arrays;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import javax.management.Attribute;
import javax.management.AttributeList;
import javax.management.AttributeNotFoundException;
import javax.management.DynamicMBean;
import javax.management.MBeanAttributeInfo;
import javax.management.MBeanInfo;

// a JMX bean that is a bag of numbers. its attributes are fixed when it's made, so the metadata the
// exporter sees never changes between scrapes. an attribute nothing has set yet reads as NaN
public class Bean implements DynamicMBean {
  private final Set<String> names;
  private final MBeanInfo info;
  private final Map<String, Double> values = new ConcurrentHashMap<>();

  public Bean(String... attributes) {
    names = new HashSet<>(Arrays.asList(attributes));
    MBeanAttributeInfo[] attrs = Arrays.stream(attributes).sorted()
      .map(n -> new MBeanAttributeInfo(n, "java.lang.Double", n, true, false, false))
      .toArray(MBeanAttributeInfo[]::new);
    info = new MBeanInfo(getClass().getName(), "server metrics", attrs, null, null, null);
  }

  public void set(String name, Number v) {
    if (!names.contains(name)) throw new IllegalArgumentException("undeclared attribute " + name);
    values.put(name, v.doubleValue());
  }

  @Override public Object getAttribute(String name) throws AttributeNotFoundException {
    if (!names.contains(name)) throw new AttributeNotFoundException(name);
    return values.getOrDefault(name, Double.NaN);
  }

  @Override public AttributeList getAttributes(String[] requested) {
    AttributeList l = new AttributeList();
    for (String n : requested)
      if (names.contains(n)) l.add(new Attribute(n, values.getOrDefault(n, Double.NaN)));
    return l;
  }

  @Override public MBeanInfo getMBeanInfo() { return info; }
  @Override public void setAttribute(Attribute a) {}
  @Override public AttributeList setAttributes(AttributeList l) { return new AttributeList(); }
  @Override public Object invoke(String op, Object[] p, String[] s) { return null; }
}

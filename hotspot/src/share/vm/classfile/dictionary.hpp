/*
 * Copyright (c) 2003, 2017, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_CLASSFILE_DICTIONARY_HPP
#define SHARE_VM_CLASSFILE_DICTIONARY_HPP

#include "classfile/protectionDomainCache.hpp"
#include "classfile/systemDictionary.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/oop.hpp"
#include "runtime/orderAccess.hpp"
#include "utilities/hashtable.hpp"
#include "utilities/ostream.hpp"

class DictionaryEntry;
class BoolObjectClosure;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// The data structure for the class loader data dictionaries (and the shared system
// dictionary).

class Dictionary : public Hashtable<InstanceKlass*, mtClass> {
  friend class VMStructs;

  ClassLoaderData* _loader_data;  // backpointer to owning loader
  ClassLoaderData* loader_data() const { return _loader_data; }

  DictionaryEntry* get_entry(int index, unsigned int hash, Symbol* name);

protected:
  DictionaryEntry* bucket(int i) const {
    return (DictionaryEntry*)Hashtable<InstanceKlass*, mtClass>::bucket(i);
  }

  // The following method is not MT-safe and must be done under lock.
  DictionaryEntry** bucket_addr(int i) {
    return (DictionaryEntry**)Hashtable<InstanceKlass*, mtClass>::bucket_addr(i);
  }

  void add_entry(int index, DictionaryEntry* new_entry) {
    Hashtable<InstanceKlass*, mtClass>::add_entry(index, (HashtableEntry<InstanceKlass*, mtClass>*)new_entry);
  }

  void free_entry(DictionaryEntry* entry);

  static size_t entry_size();
public:
  Dictionary(ClassLoaderData* loader_data, int table_size);
  Dictionary(ClassLoaderData* loader_data, int table_size, HashtableBucket<mtClass>* t, int number_of_entries);
  ~Dictionary();

  DictionaryEntry* new_entry(unsigned int hash, InstanceKlass* klass);

  void add_klass(int index, unsigned int hash, Symbol* class_name, InstanceKlass* obj);

  InstanceKlass* find_class(int index, unsigned int hash, Symbol* name);

  InstanceKlass* find_shared_class(int index, unsigned int hash, Symbol* name);

  // GC support
  void oops_do(OopClosure* f);
  void roots_oops_do(OopClosure* strong, OopClosure* weak);

  void classes_do(void f(InstanceKlass*));
  void classes_do(void f(InstanceKlass*, TRAPS), TRAPS);
  void all_entries_do(void f(InstanceKlass*, ClassLoaderData*));
  void classes_do(MetaspaceClosure* it);

  void unlink(BoolObjectClosure* is_alive);
  void remove_classes_in_error_state();

  // Unload classes whose defining loaders are unloaded
  void do_unloading();

  // Protection domains
  InstanceKlass* find(int index, unsigned int hash, Symbol* name, Handle protection_domain);
  bool is_valid_protection_domain(int index, unsigned int hash,
                                  Symbol* name,
                                  Handle protection_domain);
  void add_protection_domain(int index, unsigned int hash,
                             InstanceKlass* klass,
                             Handle protection_domain, TRAPS);

  // Sharing support
  void reorder_dictionary_for_sharing();

  void print_on(outputStream* st) const;
  void verify();
};

// An entry in the class loader data dictionaries, this describes a class as
// { InstanceKlass*, protection_domain }.

class DictionaryEntry : public HashtableEntry<InstanceKlass*, mtClass> {
  friend class VMStructs;
 private:
  // Contains the set of approved protection domains that can access
  // this dictionary entry.
  //
  // This protection domain set is a set of tuples:
  //
  // (InstanceKlass C, initiating class loader ICL, Protection Domain PD)
  //
  // [Note that C.protection_domain(), which is stored in the java.lang.Class
  // mirror of C, is NOT the same as PD]
  //
  // If such an entry (C, ICL, PD) exists in the table, it means that
  // it is okay for a class Foo to reference C, where
  //
  //    Foo.protection_domain() == PD, and
  //    Foo's defining class loader == ICL
  //
  // The usage of the PD set can be seen in SystemDictionary::validate_protection_domain()
  // It is essentially a cache to avoid repeated Java up-calls to
  // ClassLoader.checkPackageAccess().
  //
  ProtectionDomainEntry* volatile _pd_set;

 public:
  // Tells whether a protection is in the approved set.
  bool contains_protection_domain(oop protection_domain) const;
  // Adds a protection domain to the approved set.
  void add_protection_domain(Dictionary* dict, Handle protection_domain);

  InstanceKlass* instance_klass() const { return literal(); }
  InstanceKlass** klass_addr() { return (InstanceKlass**)literal_addr(); }

  DictionaryEntry* next() const {
    return (DictionaryEntry*)HashtableEntry<InstanceKlass*, mtClass>::next();
  }

  DictionaryEntry** next_addr() {
    return (DictionaryEntry**)HashtableEntry<InstanceKlass*, mtClass>::next_addr();
  }

  ProtectionDomainEntry* pd_set() const            { return _pd_set; }
  void set_pd_set(ProtectionDomainEntry* new_head) {  _pd_set = new_head; }

  ProtectionDomainEntry* pd_set_acquire() const    {
    return (ProtectionDomainEntry*)OrderAccess::load_ptr_acquire(&_pd_set);
  }
  void release_set_pd_set(ProtectionDomainEntry* new_head) {
    OrderAccess::release_store_ptr(&_pd_set, new_head);
  }

  // Tells whether the initiating class' protection domain can access the klass in this entry
  bool is_valid_protection_domain(Handle protection_domain) {
    if (!ProtectionDomainVerification) return true;
    if (!SystemDictionary::has_checkPackageAccess()) return true;

    return protection_domain() == NULL
         ? true
         : contains_protection_domain(protection_domain());
  }

  void verify_protection_domain_set() {
    for (ProtectionDomainEntry* current = pd_set(); // accessed at a safepoint
                                current != NULL;
                                current = current->_next) {
      current->_pd_cache->protection_domain()->verify();
    }
  }

  bool equals(const Symbol* class_name) const {
    InstanceKlass* klass = (InstanceKlass*)literal();
    return (klass->name() == class_name);
  }

  void print_count(outputStream *st) {
    int count = 0;
    for (ProtectionDomainEntry* current = pd_set();  // accessed inside SD lock
                                current != NULL;
                                current = current->_next) {
      count++;
    }
    st->print_cr("pd set count = #%d", count);
  }

  void verify();
};

// Entry in a SymbolPropertyTable, mapping a single Symbol*
// to a managed and an unmanaged pointer.
class SymbolPropertyEntry : public HashtableEntry<Symbol*, mtSymbol> {
  friend class VMStructs;
 private:
  intptr_t _symbol_mode;  // secondary key
  Method*   _method;
  oop       _method_type;

 public:
  Symbol* symbol() const            { return literal(); }

  intptr_t symbol_mode() const      { return _symbol_mode; }
  void set_symbol_mode(intptr_t m)  { _symbol_mode = m; }

  Method*        method() const     { return _method; }
  void set_method(Method* p)        { _method = p; }

  oop      method_type() const      { return _method_type; }
  oop*     method_type_addr()       { return &_method_type; }
  void set_method_type(oop p)       { _method_type = p; }

  SymbolPropertyEntry* next() const {
    return (SymbolPropertyEntry*)HashtableEntry<Symbol*, mtSymbol>::next();
  }

  SymbolPropertyEntry** next_addr() {
    return (SymbolPropertyEntry**)HashtableEntry<Symbol*, mtSymbol>::next_addr();
  }

  void print_entry(outputStream* st) const {
    symbol()->print_value_on(st);
    st->print("/mode=" INTX_FORMAT, symbol_mode());
    st->print(" -> ");
    bool printed = false;
    if (method() != NULL) {
      method()->print_value_on(st);
      printed = true;
    }
    if (method_type() != NULL) {
      if (printed)  st->print(" and ");
      st->print(INTPTR_FORMAT, p2i((void *)method_type()));
      printed = true;
    }
    st->print_cr(printed ? "" : "(empty)");
  }
};

// A system-internal mapping of symbols to pointers, both managed
// and unmanaged.  Used to record the auto-generation of each method
// MethodHandle.invoke(S)T, for all signatures (S)T.
class SymbolPropertyTable : public Hashtable<Symbol*, mtSymbol> {
  friend class VMStructs;
private:
  SymbolPropertyEntry* bucket(int i) {
    return (SymbolPropertyEntry*) Hashtable<Symbol*, mtSymbol>::bucket(i);
  }

  // The following method is not MT-safe and must be done under lock.
  SymbolPropertyEntry** bucket_addr(int i) {
    return (SymbolPropertyEntry**) Hashtable<Symbol*, mtSymbol>::bucket_addr(i);
  }

  void add_entry(int index, SymbolPropertyEntry* new_entry) {
    ShouldNotReachHere();
  }
  void set_entry(int index, SymbolPropertyEntry* new_entry) {
    ShouldNotReachHere();
  }

  SymbolPropertyEntry* new_entry(unsigned int hash, Symbol* symbol, intptr_t symbol_mode) {
    SymbolPropertyEntry* entry = (SymbolPropertyEntry*) Hashtable<Symbol*, mtSymbol>::new_entry(hash, symbol);
    // Hashtable with Symbol* literal must increment and decrement refcount.
    symbol->increment_refcount();
    entry->set_symbol_mode(symbol_mode);
    entry->set_method(NULL);
    entry->set_method_type(NULL);
    return entry;
  }

public:
  SymbolPropertyTable(int table_size);
  SymbolPropertyTable(int table_size, HashtableBucket<mtSymbol>* t, int number_of_entries);

  void free_entry(SymbolPropertyEntry* entry) {
    // decrement Symbol refcount here because hashtable doesn't.
    entry->literal()->decrement_refcount();
    Hashtable<Symbol*, mtSymbol>::free_entry(entry);
  }

  unsigned int compute_hash(Symbol* sym, intptr_t symbol_mode) {
    // Use the regular identity_hash.
    return Hashtable<Symbol*, mtSymbol>::compute_hash(sym) ^ symbol_mode;
  }

  int index_for(Symbol* name, intptr_t symbol_mode) {
    return hash_to_index(compute_hash(name, symbol_mode));
  }

  // need not be locked; no state change
  SymbolPropertyEntry* find_entry(int index, unsigned int hash, Symbol* name, intptr_t name_mode);

  // must be done under SystemDictionary_lock
  SymbolPropertyEntry* add_entry(int index, unsigned int hash, Symbol* name, intptr_t name_mode);

  // GC support
  void oops_do(OopClosure* f);

  void methods_do(void f(Method*));

  void verify();
};
#endif // SHARE_VM_CLASSFILE_DICTIONARY_HPP

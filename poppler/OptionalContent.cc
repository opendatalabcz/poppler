//========================================================================
//
// OptionalContent.cc
//
// Copyright 2007 Brad Hards <bradh@kde.org>
// Copyright 2008 Pino Toscano <pino@kde.org>
// Copyright 2008, 2010 Carlos Garcia Campos <carlosgc@gnome.org>
// Copyright 2008, 2010, 2011 Albert Astals Cid <aacid@kde.org>
// Copyright 2008 Mark Kaplan <mkaplan@finjan.com>
//
// Released under the GPL (version 2, or later, at your option)
//
//========================================================================

#include <config.h>

#ifdef USE_GCC_PRAGMAS
#pragma implementation
#endif

#include "goo/gmem.h"
#include "goo/GooString.h"
#include "goo/GooList.h"
#include "Error.h"
// #include "PDFDocEncoding.h"
#include "OptionalContent.h"

// Max depth of nested visibility expressions.  This is used to catch
// infinite loops in the visibility expression object structure.
#define visibilityExprRecursionLimit 50

//------------------------------------------------------------------------

OCGs::OCGs(Object *ocgObject, XRef *xref) :
  m_xref(xref)
{
  // we need to parse the dictionary here, and build optionalContentGroups
  ok = gTrue;
  optionalContentGroups = new GooList();

  Object ocgList;
  ocgObject->dictLookup("OCGs", &ocgList);
  if (!ocgList.isArray()) {
    error(errSyntaxError, -1, "Expected the optional content group list, but wasn't able to find it, or it isn't an Array");
    ocgList.free();
    ok = gFalse;
    return;
  }

  // we now enumerate over the ocgList, and build up the optionalContentGroups list.
  for(int i = 0; i < ocgList.arrayGetLength(); ++i) {
    Object ocg;
    ocgList.arrayGet(i, &ocg);
    if (!ocg.isDict()) {
      ocg.free();
      break;
    }
    OptionalContentGroup *thisOptionalContentGroup = new OptionalContentGroup(ocg.getDict());
    ocg.free();
    ocgList.arrayGetNF(i, &ocg);
    // TODO: we should create a lookup map from Ref to the OptionalContentGroup
    thisOptionalContentGroup->setRef( ocg.getRef() );
    ocg.free();
    // the default is ON - we change state later, depending on BaseState, ON and OFF
    thisOptionalContentGroup->setState(OptionalContentGroup::On);
    optionalContentGroups->append(thisOptionalContentGroup);
  }

  Object defaultOcgConfig;
  ocgObject->dictLookup("D", &defaultOcgConfig);
  if (!defaultOcgConfig.isDict()) {
    error(errSyntaxError, -1, "Expected the default config, but wasn't able to find it, or it isn't a Dictionary");
    defaultOcgConfig.free();
    ocgList.free();
    ok = gFalse;
    return;
  }

  Object baseState;
  defaultOcgConfig.dictLookup("BaseState", &baseState);
  if (baseState.isName("OFF")) {
    for (int i = 0; i < optionalContentGroups->getLength(); ++i) {
      OptionalContentGroup *group;

      group = (OptionalContentGroup *)optionalContentGroups->get(i);
      group->setState(OptionalContentGroup::Off);
    }
  }
  baseState.free();

  Object on;
  defaultOcgConfig.dictLookup("ON", &on);
  if (on.isArray()) {
    // ON is an optional element
    for (int i = 0; i < on.arrayGetLength(); ++i) {
      Object reference;
      on.arrayGetNF(i, &reference);
      if (!reference.isRef()) {
	// there can be null entries
	reference.free();
	break;
      }
      OptionalContentGroup *group = findOcgByRef( reference.getRef() );
      reference.free();
      if (!group) {
	error(errSyntaxWarning, -1, "Couldn't find group for reference");
	break;
      }
      group->setState(OptionalContentGroup::On);
    }
  }
  on.free();

  Object off;
  defaultOcgConfig.dictLookup("OFF", &off);
  if (off.isArray()) {
    // OFF is an optional element
    for (int i = 0; i < off.arrayGetLength(); ++i) {
      Object reference;
      off.arrayGetNF(i, &reference);
      if (!reference.isRef()) {
	// there can be null entries
	reference.free();
	break;
      }
      OptionalContentGroup *group = findOcgByRef( reference.getRef() );
      reference.free();
      if (!group) {
	error(errSyntaxWarning, -1, "Couldn't find group for reference to set OFF");
	break;
      }
      group->setState(OptionalContentGroup::Off);
    }
  }
  off.free();

  defaultOcgConfig.dictLookup("Order", &order);
  defaultOcgConfig.dictLookup("RBGroups", &rbgroups);

  ocgList.free();
  defaultOcgConfig.free();
}

OCGs::~OCGs()
{
  deleteGooList(optionalContentGroups, OptionalContentGroup);
  order.free();
  rbgroups.free();
}


bool OCGs::hasOCGs()
{
  return ( optionalContentGroups->getLength() > 0 );
}

OptionalContentGroup* OCGs::findOcgByRef( const Ref &ref)
{
  //TODO: make this more efficient
  OptionalContentGroup *ocg = NULL;
  for (int i=0; i < optionalContentGroups->getLength(); ++i) {
    ocg = (OptionalContentGroup*)optionalContentGroups->get(i);
    if ( (ocg->getRef().num == ref.num) && (ocg->getRef().gen == ref.gen) ) {
      return ocg;
    }
  }

  error(errSyntaxWarning, -1, "Could not find a OCG with Ref ({0:d}:{1:d})", ref.num, ref.gen);

  // not found
  return NULL;
}

bool OCGs::optContentIsVisible( Object *dictRef )
{
  Object dictObj;
  Dict *dict;
  Object dictType;
  Object ocg;
  Object policy;
  Object ve;
  bool result = true;
  dictRef->fetch( m_xref, &dictObj );
  if ( ! dictObj.isDict() ) {
    error(errSyntaxWarning, -1, "Unexpected oc reference target: {0:d}", dictObj.getType() );
    dictObj.free();
    return result;
  }
  dict = dictObj.getDict();
  // printf("checking if optContent is visible\n");
  dict->lookup("Type", &dictType);
  if (dictType.isName("OCMD")) {
    if (dict->lookup("VE", &ve)->isArray()) {
      result = evalOCVisibilityExpr(&ve, 0);
    } else {
      dict->lookupNF("OCGs", &ocg);
      if (ocg.isArray()) {
        dict->lookup("P", &policy);
        if (policy.isName("AllOn")) {
          result = allOn( ocg.getArray() );
        } else if (policy.isName("AllOff")) {
          result = allOff( ocg.getArray() );
        } else if (policy.isName("AnyOff")) {
          result = anyOff( ocg.getArray() );
        } else if ( (!policy.isName()) || (policy.isName("AnyOn") ) ) {
          // this is the default
          result = anyOn( ocg.getArray() );
        }
        policy.free();
      } else if (ocg.isRef()) {
        OptionalContentGroup *oc = findOcgByRef( ocg.getRef() );
        if ( oc && oc->getState() == OptionalContentGroup::Off ) {
          result = false;
        } else {
          result = true ;
        }
      }
      ocg.free();
    }
    ve.free();
  } else if ( dictType.isName("OCG") ) {
    OptionalContentGroup* oc = findOcgByRef( dictRef->getRef() );
    if ( oc && oc->getState() == OptionalContentGroup::Off ) {
      result=false;
    }
  }
  dictType.free();
  dictObj.free();
  // printf("visibility: %s\n", result? "on" : "off");
  return result;
}

GBool OCGs::evalOCVisibilityExpr(Object *expr, int recursion) {
  OptionalContentGroup *ocg;
  Object expr2, op, obj;
  GBool ret;
  int i;

  if (recursion > visibilityExprRecursionLimit) {
    error(errSyntaxError, -1,
	  "Loop detected in optional content visibility expression");
    return gTrue;
  }
  if (expr->isRef()) {
    if ((ocg = findOcgByRef(expr->getRef()))) {
      return ocg->getState();
    }
  }
  expr->fetch(m_xref, &expr2);
  if (!expr2.isArray() || expr2.arrayGetLength() < 1) {
    error(errSyntaxError, -1,
	  "Invalid optional content visibility expression");
    expr2.free();
    return gTrue;
  }
  expr2.arrayGet(0, &op);
  if (op.isName("Not")) {
    if (expr2.arrayGetLength() == 2) {
      expr2.arrayGetNF(1, &obj);
      ret = !evalOCVisibilityExpr(&obj, recursion + 1);
      obj.free();
    } else {
      error(errSyntaxError, -1,
	    "Invalid optional content visibility expression");
      ret = gTrue;
    }
  } else if (op.isName("And")) {
    ret = gTrue;
    for (i = 1; i < expr2.arrayGetLength() && ret; ++i) {
      expr2.arrayGetNF(i, &obj);
      ret = evalOCVisibilityExpr(&obj, recursion + 1);
      obj.free();
    }
  } else if (op.isName("Or")) {
    ret = gFalse;
    for (i = 1; i < expr2.arrayGetLength() && !ret; ++i) {
      expr2.arrayGetNF(i, &obj);
      ret = evalOCVisibilityExpr(&obj, recursion + 1);
      obj.free();
    }
  } else {
    error(errSyntaxError, -1,
	  "Invalid optional content visibility expression");
    ret = gTrue;
  }
  op.free();
  expr2.free();
  return ret;
}

bool OCGs::allOn( Array *ocgArray )
{
  for (int i = 0; i < ocgArray->getLength(); ++i) {
    Object ocgItem;
    ocgArray->getNF(i, &ocgItem);
    if (ocgItem.isRef()) {
      OptionalContentGroup* oc = findOcgByRef( ocgItem.getRef() );      
      if ( oc && oc->getState() == OptionalContentGroup::Off ) {
	return false;
      }
    }
  }
  return true;
}

bool OCGs::allOff( Array *ocgArray )
{
  for (int i = 0; i < ocgArray->getLength(); ++i) {
    Object ocgItem;
    ocgArray->getNF(i, &ocgItem);
    if (ocgItem.isRef()) {
      OptionalContentGroup* oc = findOcgByRef( ocgItem.getRef() );      
      if ( oc && oc->getState() == OptionalContentGroup::On ) {
	return false;
      }
    }
  }
  return true;
}

bool OCGs::anyOn( Array *ocgArray )
{
  for (int i = 0; i < ocgArray->getLength(); ++i) {
    Object ocgItem;
    ocgArray->getNF(i, &ocgItem);
    if (ocgItem.isRef()) {
      OptionalContentGroup* oc = findOcgByRef( ocgItem.getRef() );      
      if ( oc && oc->getState() == OptionalContentGroup::On ) {
	return true;
      }
    }
  }
  return false;
}

bool OCGs::anyOff( Array *ocgArray )
{
  for (int i = 0; i < ocgArray->getLength(); ++i) {
    Object ocgItem;
    ocgArray->getNF(i, &ocgItem);
    if (ocgItem.isRef()) {
      OptionalContentGroup* oc = findOcgByRef( ocgItem.getRef() );      
      if ( oc && oc->getState() == OptionalContentGroup::Off ) {
	return true;
      }
    }
  }
  return false;
}

//------------------------------------------------------------------------

OptionalContentGroup::OptionalContentGroup(Dict *ocgDict) : m_name(NULL)
{
  Object ocgName;
  ocgDict->lookup("Name", &ocgName);
  if (!ocgName.isString()) {
    error(errSyntaxWarning, -1, "Expected the name of the OCG, but wasn't able to find it, or it isn't a String");
  } else {
    m_name = new GooString( ocgName.getString() );
  }
  ocgName.free();
}

OptionalContentGroup::OptionalContentGroup(GooString *label)
{
  m_name = label;
  m_state = On;
}

GooString* OptionalContentGroup::getName() const
{
  return m_name;
}

void OptionalContentGroup::setRef(const Ref ref)
{
  m_ref = ref;
}

Ref OptionalContentGroup::getRef() const
{
  return m_ref;
}

OptionalContentGroup::~OptionalContentGroup()
{
  delete m_name;
}


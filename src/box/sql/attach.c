/*
** 2003 April 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to implement the ATTACH and DETACH commands.
*/
#include "sqliteInt.h"

/*
** Initialize a DbFixer structure.  This routine must be called prior
** to passing the structure to one of the sqliteFixAAAA() routines below.
*/
void sqlite3FixInit(
  DbFixer *pFix,      /* The fixer to be initialized */
  Parse *pParse,      /* Error messages will be written here */
  const char *zType,  /* "view", "trigger", or "index" */
  const Token *pName  /* Name of the view, trigger, or index */
){
  sqlite3 *db;

  db = pParse->db;
  pFix->pParse = pParse;
  pFix->zDb = db->mdb.zDbSName;
  pFix->pSchema = db->mdb.pSchema;
  pFix->zType = zType;
  pFix->pName = pName;
  pFix->bVarOnly = 0;
}

/*
** The following set of routines walk through the parse tree and assign
** a specific database to all table references where the database name
** was left unspecified in the original SQL statement.  The pFix structure
** must have been initialized by a prior call to sqlite3FixInit().
**
** These routines are used to make sure that an index, trigger, or
** view in one database does not refer to objects in a different database.
** (Exception: indices, triggers, and views in the TEMP database are
** allowed to refer to anything.)  If a reference is explicitly made
** to an object in a different database, an error message is added to
** pParse->zErrMsg and these routines return non-zero.  If everything
** checks out, these routines return 0.
*/
int sqlite3FixSrcList(
  DbFixer *pFix,       /* Context of the fixation */
  SrcList *pList       /* The Source list to check and modify */
){
  int i;
  const char *zDb;
  struct SrcList_item *pItem;

  if( NEVER(pList==0) ) return 0;
  zDb = pFix->zDb;
  for(i=0, pItem=pList->a; i<pList->nSrc; i++, pItem++){
    if( pFix->bVarOnly==0 ){
      if( pItem->zDatabase && sqlite3StrICmp(pItem->zDatabase, zDb) ){
        sqlite3ErrorMsg(pFix->pParse,
            "%s %T cannot reference objects in database %s",
            pFix->zType, pFix->pName, pItem->zDatabase);
        return 1;
      }
      sqlite3DbFree(pFix->pParse->db, pItem->zDatabase);
      pItem->zDatabase = 0;
      pItem->pSchema = pFix->pSchema;
    }
#if !defined(SQLITE_OMIT_VIEW) || !defined(SQLITE_OMIT_TRIGGER)
    if( sqlite3FixSelect(pFix, pItem->pSelect) ) return 1;
    if( sqlite3FixExpr(pFix, pItem->pOn) ) return 1;
#endif
  }
  return 0;
}
#if !defined(SQLITE_OMIT_VIEW) || !defined(SQLITE_OMIT_TRIGGER)
int sqlite3FixSelect(
  DbFixer *pFix,       /* Context of the fixation */
  Select *pSelect      /* The SELECT statement to be fixed to one database */
){
  while( pSelect ){
    if( sqlite3FixExprList(pFix, pSelect->pEList) ){
      return 1;
    }
    if( sqlite3FixSrcList(pFix, pSelect->pSrc) ){
      return 1;
    }
    if( sqlite3FixExpr(pFix, pSelect->pWhere) ){
      return 1;
    }
    if( sqlite3FixExprList(pFix, pSelect->pGroupBy) ){
      return 1;
    }
    if( sqlite3FixExpr(pFix, pSelect->pHaving) ){
      return 1;
    }
    if( sqlite3FixExprList(pFix, pSelect->pOrderBy) ){
      return 1;
    }
    if( sqlite3FixExpr(pFix, pSelect->pLimit) ){
      return 1;
    }
    if( sqlite3FixExpr(pFix, pSelect->pOffset) ){
      return 1;
    }
    pSelect = pSelect->pPrior;
  }
  return 0;
}
int sqlite3FixExpr(
  DbFixer *pFix,     /* Context of the fixation */
  Expr *pExpr        /* The expression to be fixed to one database */
){
  while( pExpr ){
    if( pExpr->op==TK_VARIABLE ){
      if( pFix->pParse->db->init.busy ){
        pExpr->op = TK_NULL;
      }else{
        sqlite3ErrorMsg(pFix->pParse, "%s cannot use variables", pFix->zType);
        return 1;
      }
    }
    if( ExprHasProperty(pExpr, EP_TokenOnly|EP_Leaf) ) break;
    if( ExprHasProperty(pExpr, EP_xIsSelect) ){
      if( sqlite3FixSelect(pFix, pExpr->x.pSelect) ) return 1;
    }else{
      if( sqlite3FixExprList(pFix, pExpr->x.pList) ) return 1;
    }
    if( sqlite3FixExpr(pFix, pExpr->pRight) ){
      return 1;
    }
    pExpr = pExpr->pLeft;
  }
  return 0;
}
int sqlite3FixExprList(
  DbFixer *pFix,     /* Context of the fixation */
  ExprList *pList    /* The expression to be fixed to one database */
){
  int i;
  struct ExprList_item *pItem;
  if( pList==0 ) return 0;
  for(i=0, pItem=pList->a; i<pList->nExpr; i++, pItem++){
    if( sqlite3FixExpr(pFix, pItem->pExpr) ){
      return 1;
    }
  }
  return 0;
}
#endif

#ifndef SQLITE_OMIT_TRIGGER
int sqlite3FixTriggerStep(
  DbFixer *pFix,     /* Context of the fixation */
  TriggerStep *pStep /* The trigger step be fixed to one database */
){
  while( pStep ){
    if( sqlite3FixSelect(pFix, pStep->pSelect) ){
      return 1;
    }
    if( sqlite3FixExpr(pFix, pStep->pWhere) ){
      return 1;
    }
    if( sqlite3FixExprList(pFix, pStep->pExprList) ){
      return 1;
    }
    pStep = pStep->pNext;
  }
  return 0;
}
#endif

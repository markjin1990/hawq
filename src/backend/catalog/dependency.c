/*-------------------------------------------------------------------------
 *
 * dependency.c
 *	  Routines to support inter-object dependencies.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/dependency.c,v 1.60 2006/10/04 00:29:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/catquery.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_attribute_encoding.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_compression.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extprotocol.h"
#include "catalog/pg_filespace.h"
#include "catalog/pg_filesystem.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_partition_encoding.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/pg_type_encoding.h"
#include "catalog/pg_user_mapping.h"
#include "cdb/cdbpartition.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/extprotocolcmds.h"
#include "commands/filespace.h"
#include "commands/filesystemcmds.h"
#include "commands/proclang.h"
#include "commands/schemacmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteRemove.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "cdb/cdbvars.h"


/* expansible list of ObjectAddresses */
struct ObjectAddresses
{
	ObjectAddress *refs;		/* => palloc'd array */
	int			numrefs;		/* current number of references */
	int			maxrefs;		/* current size of palloc'd array */
};

/* typedef ObjectAddresses appears in dependency.h */

/* for find_expr_references_walker */
typedef struct
{
	ObjectAddresses *addrs;		/* addresses being accumulated */
	List	   *rtables;		/* list of rangetables to resolve Vars */
} find_expr_references_context;

/*
 * This constant table maps ObjectClasses to the corresponding catalog OIDs.
 * See also getObjectClass().
 */
static const Oid object_classes[MAX_OCLASS] = {
	RelationRelationId,			/* OCLASS_CLASS */
	ProcedureRelationId,		/* OCLASS_PROC */
	TypeRelationId,				/* OCLASS_TYPE */
	CastRelationId,				/* OCLASS_CAST */
	ConstraintRelationId,		/* OCLASS_CONSTRAINT */
	ConversionRelationId,		/* OCLASS_CONVERSION */
	AttrDefaultRelationId,		/* OCLASS_DEFAULT */
	LanguageRelationId,			/* OCLASS_LANGUAGE */
	OperatorRelationId,			/* OCLASS_OPERATOR */
	OperatorClassRelationId,	/* OCLASS_OPCLASS */
	RewriteRelationId,			/* OCLASS_REWRITE */
	TriggerRelationId,			/* OCLASS_TRIGGER */
	NamespaceRelationId,		/* OCLASS_SCHEMA */
	AuthIdRelationId,           /* OCLASS_ROLE */
	DatabaseRelationId,         /* OCLASS_DATABASE */
	TableSpaceRelationId,       /* OCLASS_TBLSPACE */
	FileSpaceRelationId,        /* OCLASS_FILESPACE */
	FileSystemRelationId,       /* OCLASS_FILESYSTEM */
	ForeignDataWrapperRelationId,	/* OCLASS_FDW */
	ForeignServerRelationId,	/* OCLASS_FOREIGN_SERVER */
	UserMappingRelationId,		/* OCLASS_USER_MAPPING */
	ExtprotocolRelationId,		/* OCLASS_EXTPROTOCOL */
	CompressionRelationId		/* OCLASS_COMPRESSION */
};


static void performDeletionWithList(const ObjectAddress *object,
						ObjectAddresses *oktodelete,
						DropBehavior behavior,
						ObjectAddresses *alreadyDeleted);
static void findAutoDeletableObjects(const ObjectAddress *object,
						 ObjectAddresses *oktodelete,
						 Relation depRel, bool addself);
static bool recursiveDeletion(const ObjectAddress *object,
				  DropBehavior behavior,
				  int msglevel,
				  const ObjectAddress *callingObject,
				  ObjectAddresses *oktodelete,
				  Relation depRel,
				  ObjectAddresses *alreadyDeleted);
static bool deleteDependentObjects(const ObjectAddress *object,
					   const char *objDescription,
					   DropBehavior behavior,
					   int msglevel,
					   ObjectAddresses *oktodelete,
					   Relation depRel,
					   ObjectAddresses *alreadyDeleted);
static void doDeletion(const ObjectAddress *object);
static bool find_expr_references_walker(Node *node,
							find_expr_references_context *context);
static void eliminate_duplicate_dependencies(ObjectAddresses *addrs);
static int	object_address_comparator(const void *a, const void *b);
static void add_object_address(ObjectClass oclass, Oid objectId, int32 subId,
				   ObjectAddresses *addrs);
static void getRelationDescription(StringInfo buffer, Oid relid);


/*
 * performDeletion: attempt to drop the specified object.  If CASCADE
 * behavior is specified, also drop any dependent objects (recursively).
 * If RESTRICT behavior is specified, error out if there are any dependent
 * objects, except for those that should be implicitly dropped anyway
 * according to the dependency type.
 *
 * This is the outer control routine for all forms of DROP that drop objects
 * that can participate in dependencies.
 */
void
performDeletion(const ObjectAddress *object,
				DropBehavior behavior)
{
	char	   *objDescription;
	Relation	depRel;
	ObjectAddresses *oktodelete;

	/*
	 * Get object description for possible use in failure message. Must do
	 * this before deleting it ...
	 */
	objDescription = getObjectDescription(object);

	/*
	 * We save some cycles by opening pg_depend just once and passing the
	 * Relation pointer down to all the recursive deletion steps.
	 */
	depRel = heap_open(DependRelationId, RowExclusiveLock);

	/*
	 * Construct a list of objects that are reachable by AUTO or INTERNAL
	 * dependencies from the target object.  These should be deleted silently,
	 * even if the actual deletion pass first reaches one of them via a
	 * non-auto dependency.
	 */
	oktodelete = new_object_addresses();

	findAutoDeletableObjects(object, oktodelete, depRel, true);

	if (!recursiveDeletion(object, behavior, NOTICE,
						   NULL, oktodelete, depRel, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot drop %s because other objects depend on it",
						objDescription),
		errhint("Use DROP ... CASCADE to drop the dependent objects too."),
		errOmitLocation(true)));

	free_object_addresses(oktodelete);

	heap_close(depRel, RowExclusiveLock);

	pfree(objDescription);
}


/*
 * performDeletionWithList: As above, but the oktodelete list may have already
 * filled with some objects.  Also, the deleted objects are saved in the
 * alreadyDeleted list.
 *
 * XXX performDeletion could be refactored to be a thin wrapper to this
 * function.
 */
static void
performDeletionWithList(const ObjectAddress *object,
						ObjectAddresses *oktodelete,
						DropBehavior behavior,
						ObjectAddresses *alreadyDeleted)
{
	char	   *objDescription;
	Relation	depRel;

	/*
	 * Get object description for possible use in failure message. Must do
	 * this before deleting it ...
	 */
	objDescription = getObjectDescription(object);

	/*
	 * We save some cycles by opening pg_depend just once and passing the
	 * Relation pointer down to all the recursive deletion steps.
	 */
	depRel = heap_open(DependRelationId, RowExclusiveLock);

	/*
	 * Construct a list of objects that are reachable by AUTO or INTERNAL
	 * dependencies from the target object.  These should be deleted silently,
	 * even if the actual deletion pass first reaches one of them via a
	 * non-auto dependency.
	 */
	findAutoDeletableObjects(object, oktodelete, depRel, true);

	if (!recursiveDeletion(object, behavior, NOTICE,
						   NULL, oktodelete, depRel, alreadyDeleted))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot drop %s because other objects depend on it",
						objDescription),
		errhint("Use DROP ... CASCADE to drop the dependent objects too."),
		errOmitLocation(true)));

	heap_close(depRel, RowExclusiveLock);

	pfree(objDescription);
}

/*
 * performMultipleDeletion: Similar to performDeletion, but act on multiple
 * objects at once.
 *
 * The main difference from issuing multiple performDeletion calls is that the
 * list of objects that would be implicitly dropped, for each object to be
 * dropped, is the union of the implicit-object list for all objects.  This
 * makes each check be more relaxed.
 */
void
performMultipleDeletions(const ObjectAddresses *objects,
						 DropBehavior behavior)
{
	ObjectAddresses *implicit;
	ObjectAddresses *alreadyDeleted;
	Relation	depRel;
	int			i;

	implicit = new_object_addresses();
	alreadyDeleted = new_object_addresses();

	depRel = heap_open(DependRelationId, RowExclusiveLock);

	/*
	 * Get the list of all objects that would be deleted after deleting the
	 * whole "objects" list.  We do this by creating a list of all implicit
	 * (INTERNAL and AUTO) dependencies for each object we collected above.
	 * Note that we must exclude the objects themselves from this list!
	 */
	for (i = 0; i < objects->numrefs; i++)
	{
		ObjectAddress obj = objects->refs[i];

		/*
		 * If it's in the implicit list, we don't need to delete it explicitly
		 * nor follow the dependencies, because that was already done in a
		 * previous iteration.
		 */
		if (object_address_present(&obj, implicit))
			continue;

		/*
		 * Add the objects dependent on this one to the global list of
		 * implicit objects.
		 */
		findAutoDeletableObjects(&obj, implicit, depRel, false);
	}

	/* Do the deletion. */
	for (i = 0; i < objects->numrefs; i++)
	{
		ObjectAddress obj = objects->refs[i];

		/*
		 * Skip this object if it was already deleted in a previous iteration.
		 */
		if (object_address_present(&obj, alreadyDeleted))
			continue;

		/*
		 * Skip this object if it's also present in the list of implicit
		 * objects --- it will be deleted later.
		 */
		if (object_address_present(&obj, implicit))
			continue;

		/* delete it */
		performDeletionWithList(&obj, implicit, behavior, alreadyDeleted);
	}

	heap_close(depRel, RowExclusiveLock);

	free_object_addresses(implicit);
	free_object_addresses(alreadyDeleted);
}

/*
 * deleteWhatDependsOn: attempt to drop everything that depends on the
 * specified object, though not the object itself.	Behavior is always
 * CASCADE.
 *
 * This is currently used only to clean out the contents of a schema
 * (namespace): the passed object is a namespace.  We normally want this
 * to be done silently, so there's an option to suppress NOTICE messages.
 */
void
deleteWhatDependsOn(const ObjectAddress *object,
					bool showNotices)
{
	char	   *objDescription;
	Relation	depRel;
	ObjectAddresses *oktodelete;

	/*
	 * Get object description for possible use in failure messages
	 */
	objDescription = getObjectDescription(object);

	/*
	 * We save some cycles by opening pg_depend just once and passing the
	 * Relation pointer down to all the recursive deletion steps.
	 */
	depRel = heap_open(DependRelationId, RowExclusiveLock);

	/*
	 * Construct a list of objects that are reachable by AUTO or INTERNAL
	 * dependencies from the target object.  These should be deleted silently,
	 * even if the actual deletion pass first reaches one of them via a
	 * non-auto dependency.
	 */
	oktodelete = new_object_addresses();

	findAutoDeletableObjects(object, oktodelete, depRel, true);

	/*
	 * Now invoke only step 2 of recursiveDeletion: just recurse to the stuff
	 * dependent on the given object.
	 */
	if (!deleteDependentObjects(object, objDescription,
								DROP_CASCADE,
								showNotices ? NOTICE : DEBUG2,
								oktodelete, depRel, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("failed to drop all objects depending on %s",
						objDescription)));

	/*
	 * We do not need CommandCounterIncrement here, since if step 2 did
	 * anything then each recursive call will have ended with one.
	 */

	free_object_addresses(oktodelete);

	heap_close(depRel, RowExclusiveLock);

	pfree(objDescription);
}


/*
 * findAutoDeletableObjects: find all objects that are reachable by AUTO or
 * INTERNAL dependency paths from the given object.  Add them all to the
 * oktodelete list.  If addself is true, the originally given object will also
 * be added to the list.
 *
 * depRel is the already-open pg_depend relation.
 */
static void
findAutoDeletableObjects(const ObjectAddress *object,
						 ObjectAddresses *oktodelete,
						 Relation depRel, bool addself)
{
	HeapTuple	tup;
	cqContext  *pcqCtx;
	cqContext	cqc;
	ObjectAddress otherObject;

	/*
	 * If this object is already in oktodelete, then we already visited it;
	 * don't do so again (this prevents infinite recursion if there's a loop
	 * in pg_depend).  Otherwise, add it.
	 */
	if (object_address_present(object, oktodelete))
		return;
	if (addself)
		add_exact_object_address(object, oktodelete);

	/*
	 * Scan pg_depend records that link to this object, showing the things
	 * that depend on it.  For each one that is AUTO or INTERNAL, visit the
	 * referencing object.
	 *
	 * When dropping a whole object (subId = 0), find pg_depend records for
	 * its sub-objects too.
	 */
	if (object->objectSubId != 0)
	{
		pcqCtx = caql_beginscan(
				caql_addrel(cqclr(&cqc), depRel),
				cql("SELECT * FROM pg_depend "
					" WHERE refclassid = :1 "
					" AND refobjid = :2 "
					" AND refobjsubid = :3 ",
					ObjectIdGetDatum(object->classId),
					ObjectIdGetDatum(object->objectId),
					Int32GetDatum(object->objectSubId)));
	}
	else
	{
		pcqCtx = caql_beginscan(
				caql_addrel(cqclr(&cqc), depRel),
				cql("SELECT * FROM pg_depend "
					" WHERE refclassid = :1 "
					" AND refobjid = :2 ",
					ObjectIdGetDatum(object->classId),
					ObjectIdGetDatum(object->objectId)));
	}

	while (HeapTupleIsValid(tup = caql_getnext(pcqCtx)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);

		switch (foundDep->deptype)
		{
			case DEPENDENCY_NORMAL:
				/* ignore */
				break;
			case DEPENDENCY_AUTO:
			case DEPENDENCY_INTERNAL:
				/* recurse */
				otherObject.classId = foundDep->classid;
				otherObject.objectId = foundDep->objid;
				otherObject.objectSubId = foundDep->objsubid;
				findAutoDeletableObjects(&otherObject, oktodelete, depRel, true);
				break;
			case DEPENDENCY_PIN:

				/*
				 * For a PIN dependency we just ereport immediately; there
				 * won't be any others to examine, and we aren't ever going to
				 * let the user delete it.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("cannot drop %s because it is required by the database system",
								getObjectDescription(object))));
				break;
			default:
				elog(ERROR, "unrecognized dependency type '%c' for %s",
					 foundDep->deptype, getObjectDescription(object));
				break;
		}
	}

	caql_endscan(pcqCtx);
}


/*
 * recursiveDeletion: delete a single object for performDeletion, plus
 * (recursively) anything that depends on it.
 *
 * Returns TRUE if successful, FALSE if not.
 *
 * callingObject is NULL at the outer level, else identifies the object that
 * we recursed from (the reference object that someone else needs to delete).
 *
 * oktodelete is a list of objects verified deletable (ie, reachable by one
 * or more AUTO or INTERNAL dependencies from the original target).
 *
 * depRel is the already-open pg_depend relation.
 *
 * alreadyDeleted is a list to add objects to as they are deleted, or NULL
 * if the caller doesn't need to have such a list.
 *
 *
 * In RESTRICT mode, we perform all the deletions anyway, but ereport a message
 * and return FALSE if we find a restriction violation.  performDeletion
 * will then abort the transaction to nullify the deletions.  We have to
 * do it this way to (a) report all the direct and indirect dependencies
 * while (b) not going into infinite recursion if there's a cycle.
 *
 * This is even more complex than one could wish, because it is possible for
 * the same pair of objects to be related by both NORMAL and AUTO/INTERNAL
 * dependencies.  Also, we might have a situation where we've been asked to
 * delete object A, and objects B and C both have AUTO dependencies on A,
 * but B also has a NORMAL dependency on C.  (Since any of these paths might
 * be indirect, we can't prevent these scenarios, but must cope instead.)
 * If we visit C before B then we would mistakenly decide that the B->C link
 * should prevent the restricted drop from occurring.  To handle this, we make
 * a pre-scan to find all the objects that are auto-deletable from A.  If we
 * visit C first, but B is present in the oktodelete list, then we make no
 * complaint but recurse to delete B anyway.  (Note that in general we must
 * delete B before deleting C; the drop routine for B may try to access C.)
 *
 * Note: in the case where the path to B is traversed first, we will not
 * see the NORMAL dependency when we reach C, because of the pg_depend
 * removals done in step 1.  The oktodelete list is necessary just
 * to make the behavior independent of the order in which pg_depend
 * entries are visited.
 */
static bool
recursiveDeletion(const ObjectAddress *object,
				  DropBehavior behavior,
				  int msglevel,
				  const ObjectAddress *callingObject,
				  ObjectAddresses *oktodelete,
				  Relation depRel,
				  ObjectAddresses *alreadyDeleted)
{
	bool		ok = true;
	char	   *objDescription;
	HeapTuple	tup;
	cqContext  *pcqCtx;
	cqContext	cqc;
	ObjectAddress otherObject;
	ObjectAddress owningObject;
	bool		amOwned = false;

	/*
	 * Get object description for possible use in messages.  Must do this
	 * before deleting it ...
	 */
	objDescription = getObjectDescription(object);

	/*
	 * Step 1: find and remove pg_depend records that link from this object to
	 * others.	We have to do this anyway, and doing it first ensures that we
	 * avoid infinite recursion in the case of cycles. Also, some dependency
	 * types require extra processing here.
	 *
	 * When dropping a whole object (subId = 0), remove all pg_depend records
	 * for its sub-objects too.
	 */
	if (object->objectSubId != 0)
	{
		pcqCtx = caql_beginscan(
				caql_addrel(cqclr(&cqc), depRel),
				cql("SELECT * FROM pg_depend "
					" WHERE classid = :1 "
					" AND objid = :2 "
					" AND objsubid = :3 "
					" FOR UPDATE ",
					ObjectIdGetDatum(object->classId),
					ObjectIdGetDatum(object->objectId),
					Int32GetDatum(object->objectSubId)));
	}
	else
	{
		pcqCtx = caql_beginscan(
				caql_addrel(cqclr(&cqc), depRel),
				cql("SELECT * FROM pg_depend "
					" WHERE classid = :1 "
					" AND objid = :2 "
					" FOR UPDATE ",
					ObjectIdGetDatum(object->classId),
					ObjectIdGetDatum(object->objectId)));
	}

	while (HeapTupleIsValid(tup = caql_getnext(pcqCtx)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);

		otherObject.classId = foundDep->refclassid;
		otherObject.objectId = foundDep->refobjid;
		otherObject.objectSubId = foundDep->refobjsubid;

		switch (foundDep->deptype)
		{
			case DEPENDENCY_NORMAL:
			case DEPENDENCY_AUTO:
				/* no problem */
				break;
			case DEPENDENCY_INTERNAL:

				/*
				 * This object is part of the internal implementation of
				 * another object.	We have three cases:
				 *
				 * 1. At the outermost recursion level, disallow the DROP. (We
				 * just ereport here, rather than proceeding, since no other
				 * dependencies are likely to be interesting.)
				 */
				if (callingObject == NULL)
				{
					char	   *otherObjDesc = getObjectDescription(&otherObject);

					ereport(ERROR,
							(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
							 errmsg("cannot drop %s because %s requires it",
									objDescription, otherObjDesc),
							 errhint("You may drop %s instead.",
									 otherObjDesc),
							 errOmitLocation(true)));
				}

				/*
				 * 2. When recursing from the other end of this dependency,
				 * it's okay to continue with the deletion. This holds when
				 * recursing from a whole object that includes the nominal
				 * other end as a component, too.
				 */
				if (callingObject->classId == otherObject.classId &&
					callingObject->objectId == otherObject.objectId &&
					(callingObject->objectSubId == otherObject.objectSubId ||
					 callingObject->objectSubId == 0))
					break;

				/*
				 * 3. When recursing from anyplace else, transform this
				 * deletion request into a delete of the other object. (This
				 * will be an error condition iff RESTRICT mode.) In this case
				 * we finish deleting my dependencies except for the INTERNAL
				 * link, which will be needed to cause the owning object to
				 * recurse back to me.
				 */
				if (amOwned)	/* shouldn't happen */
					elog(ERROR, "multiple INTERNAL dependencies for %s",
						 objDescription);
				owningObject = otherObject;
				amOwned = true;
				/* "continue" bypasses the caql_delete call below */
				continue;
			case DEPENDENCY_PIN:

				/*
				 * Should not happen; PIN dependencies should have zeroes in
				 * the depender fields...
				 */
				elog(ERROR, "incorrect use of PIN dependency with %s",
					 objDescription);
				break;
			default:
				elog(ERROR, "unrecognized dependency type '%c' for %s",
					 foundDep->deptype, objDescription);
				break;
		}

		/* delete the pg_depend tuple */
		caql_delete_current(pcqCtx);
	}

	caql_endscan(pcqCtx);

	/*
	 * CommandCounterIncrement here to ensure that preceding changes are all
	 * visible; in particular, that the above deletions of pg_depend entries
	 * are visible.  That prevents infinite recursion in case of a dependency
	 * loop (which is perfectly legal).
	 */
	CommandCounterIncrement();

	/*
	 * If we found we are owned by another object, ask it to delete itself
	 * instead of proceeding.  Complain if RESTRICT mode, unless the other
	 * object is in oktodelete.
	 */
	if (amOwned)
	{
		if (object_address_present(&owningObject, oktodelete))
			ereport(DEBUG2,
					(errmsg("drop auto-cascades to %s",
							getObjectDescription(&owningObject))));
		else if (behavior == DROP_RESTRICT)
		{
			if (msglevel == NOTICE && Gp_role == GP_ROLE_EXECUTE)
			ereport(DEBUG1,
					(errmsg("%s depends on %s",
							getObjectDescription(&owningObject),
							objDescription),
					 errOmitLocation(true)));
			else
			ereport(msglevel,
					(errmsg("%s depends on %s",
							getObjectDescription(&owningObject),
							objDescription)));
			ok = false;
		}
		else
		{
			if (Gp_role == GP_ROLE_EXECUTE)
			ereport(DEBUG1,
					(errmsg("drop cascades to %s",
							getObjectDescription(&owningObject))));
			else
			ereport(msglevel,
					(errmsg("drop cascades to %s",
							getObjectDescription(&owningObject))));
		}

		if (!recursiveDeletion(&owningObject, behavior, msglevel,
							   object, oktodelete, depRel, alreadyDeleted))
			ok = false;

		pfree(objDescription);

		return ok;
	}

	/*
	 * Step 2: scan pg_depend records that link to this object, showing the
	 * things that depend on it.  Recursively delete those things. Note it's
	 * important to delete the dependent objects before the referenced one,
	 * since the deletion routines might do things like try to update the
	 * pg_class record when deleting a check constraint.
	 */
	if (!deleteDependentObjects(object, objDescription,
								behavior, msglevel,
								oktodelete, depRel, alreadyDeleted))
		ok = false;

	/*
	 * We do not need CommandCounterIncrement here, since if step 2 did
	 * anything then each recursive call will have ended with one.
	 */

	/*
	 * Step 3: delete the object itself, and save it to the list of deleted
	 * objects if appropiate.
	 */
	doDeletion(object);
	if (alreadyDeleted != NULL)
	{
		if (!object_address_present(object, alreadyDeleted))
			add_exact_object_address(object, alreadyDeleted);
	}

	/*
	 * Delete any comments associated with this object.  (This is a convenient
	 * place to do it instead of having every object type know to do it.)
	 */
	DeleteComments(object->objectId, object->classId, object->objectSubId);

	/*
	 * Delete shared dependency references related to this object. Sub-objects
	 * (columns) don't have dependencies on global objects, so skip them.
	 */
	if (object->objectSubId == 0)
		deleteSharedDependencyRecordsFor(object->classId, object->objectId);

	/*
	 * CommandCounterIncrement here to ensure that preceding changes are all
	 * visible.
	 */
	CommandCounterIncrement();

	/*
	 * And we're done!
	 */
	pfree(objDescription);

	return ok;
}


/*
 * deleteDependentObjects - find and delete objects that depend on 'object'
 *
 * Scan pg_depend records that link to the given object, showing
 * the things that depend on it.  Recursively delete those things. (We
 * don't delete the pg_depend records here, as the recursive call will
 * do that.)  Note it's important to delete the dependent objects
 * before the referenced one, since the deletion routines might do
 * things like try to update the pg_class record when deleting a check
 * constraint.
 *
 * When dropping a whole object (subId = 0), find pg_depend records for
 * its sub-objects too.
 *
 *	object: the object to find dependencies on
 *	objDescription: description of object (only used for error messages)
 *	behavior: desired drop behavior
 *	oktodelete: stuff that's AUTO-deletable
 *	depRel: already opened pg_depend relation
 *	alreadyDeleted: optional list to add deleted objects to
 *
 * Returns TRUE if all is well, false if any problem found.
 *
 * NOTE: because we are using SnapshotNow, if a recursive call deletes
 * any pg_depend tuples that our scan hasn't yet visited, we will not
 * see them as good when we do visit them.	This is essential for
 * correct behavior if there are multiple dependency paths between two
 * objects --- else we might try to delete an already-deleted object.
 */
static bool
deleteDependentObjects(const ObjectAddress *object,
					   const char *objDescription,
					   DropBehavior behavior,
					   int msglevel,
					   ObjectAddresses *oktodelete,
					   Relation depRel,
					   ObjectAddresses *alreadyDeleted)
{
	bool		ok = true;
	HeapTuple	tup;
	cqContext  *pcqCtx;
	cqContext	cqc;
	ObjectAddress otherObject;

	if (object->objectSubId != 0)
	{
		pcqCtx = caql_beginscan(
				caql_addrel(cqclr(&cqc), depRel),
				cql("SELECT * FROM pg_depend "
					" WHERE refclassid = :1 "
					" AND refobjid = :2 "
					" AND refobjsubid = :3 "
					" FOR UPDATE ",
					ObjectIdGetDatum(object->classId),
					ObjectIdGetDatum(object->objectId),
					Int32GetDatum(object->objectSubId)));
	}
	else
	{
		pcqCtx = caql_beginscan(
				caql_addrel(cqclr(&cqc), depRel),
				cql("SELECT * FROM pg_depend "
					" WHERE refclassid = :1 "
					" AND refobjid = :2 "
					" FOR UPDATE ",
					ObjectIdGetDatum(object->classId),
					ObjectIdGetDatum(object->objectId)));
	}

	while (HeapTupleIsValid(tup = caql_getnext(pcqCtx)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);

		otherObject.classId = foundDep->classid;
		otherObject.objectId = foundDep->objid;
		otherObject.objectSubId = foundDep->objsubid;

		switch (foundDep->deptype)
		{
			case DEPENDENCY_NORMAL:

				/*
				 * Perhaps there was another dependency path that would have
				 * allowed silent deletion of the otherObject, had we only
				 * taken that path first. In that case, act like this link is
				 * AUTO, too.
				 */
				if (object_address_present(&otherObject, oktodelete))
					ereport(DEBUG2,
							(errmsg("drop auto-cascades to %s",
									getObjectDescription(&otherObject))));
				else if (behavior == DROP_RESTRICT)
				{
					if (msglevel == NOTICE && Gp_role == GP_ROLE_EXECUTE)
					ereport(DEBUG1,
							(errmsg("%s depends on %s",
									getObjectDescription(&otherObject),
									objDescription)));
					else
					ereport(msglevel,
							(errmsg("%s depends on %s",
									getObjectDescription(&otherObject),
									objDescription)));
					ok = false;
				}
				else
				{
					if (Gp_role == GP_ROLE_EXECUTE)
					ereport(DEBUG1,
							(errmsg("drop cascades to %s",
									getObjectDescription(&otherObject))));
					else
					ereport(msglevel,
							(errmsg("drop cascades to %s",
									getObjectDescription(&otherObject))));
				}

				if (!recursiveDeletion(&otherObject, behavior, msglevel,
									   object, oktodelete, depRel,
									   alreadyDeleted))
					ok = false;
				break;
			case DEPENDENCY_AUTO:
			case DEPENDENCY_INTERNAL:

				/*
				 * We propagate the DROP without complaint even in the
				 * RESTRICT case.  (However, normal dependencies on the
				 * component object could still cause failure.)
				 */
				ereport(DEBUG2,
						(errmsg("drop auto-cascades to %s",
								getObjectDescription(&otherObject))));

				if (!recursiveDeletion(&otherObject, behavior, msglevel,
									   object, oktodelete, depRel,
									   alreadyDeleted))
					ok = false;
				break;
			case DEPENDENCY_PIN:

				/*
				 * For a PIN dependency we just ereport immediately; there
				 * won't be any others to report.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("cannot drop %s because it is required by the database system",
								objDescription)));
				break;
			default:
				elog(ERROR, "unrecognized dependency type '%c' for %s",
					 foundDep->deptype, objDescription);
				break;
		}
	}

	caql_endscan(pcqCtx);

	return ok;
}


/*
 * doDeletion: actually delete a single object
 */
static void
doDeletion(const ObjectAddress *object)
{
	switch (getObjectClass(object))
	{
		case OCLASS_CLASS:
			{
				char		relKind = get_rel_relkind(object->objectId);

				if (relKind == RELKIND_INDEX)
				{
					Assert(object->objectSubId == 0);
					index_drop(object->objectId);
				}
				else
				{
					if (object->objectSubId != 0)
						RemoveAttributeById(object->objectId,
											object->objectSubId);
					else
						heap_drop_with_catalog(object->objectId);
				}
				break;
			}

		case OCLASS_PROC:
			RemoveFunctionById(object->objectId);
			break;

		case OCLASS_TYPE:
			RemoveTypeById(object->objectId);
			break;

		case OCLASS_CAST:
			DropCastById(object->objectId);
			break;

		case OCLASS_CONSTRAINT:
			RemoveConstraintById(object->objectId);
			break;

		case OCLASS_CONVERSION:
			RemoveConversionById(object->objectId);
			break;

		case OCLASS_DEFAULT:
			RemoveAttrDefaultById(object->objectId);
			break;

		case OCLASS_LANGUAGE:
			DropProceduralLanguageById(object->objectId);
			break;

		case OCLASS_OPERATOR:
			RemoveOperatorById(object->objectId);
			break;

		case OCLASS_OPCLASS:
			RemoveOpClassById(object->objectId);
			break;

		case OCLASS_REWRITE:
			RemoveRewriteRuleById(object->objectId);
			break;

		case OCLASS_TRIGGER:
			RemoveTriggerById(object->objectId);
			break;

		case OCLASS_SCHEMA:
			RemoveSchemaById(object->objectId);
			break;

		case OCLASS_FILESPACE:
			RemoveFileSpaceById(object->objectId);
			break;

		case OCLASS_FILESYSTEM:
			RemoveFileSystemById(object->objectId);
			break;

			/*
			 * OCLASS_ROLE, OCLASS_DATABASE, OCLASS_TBLSPACE intentionally
			 * not handled here
			 */

		case OCLASS_FDW:
			RemoveForeignDataWrapperById(object->objectId);
			break;

		case OCLASS_FOREIGN_SERVER:
			RemoveForeignServerById(object->objectId);
			break;

		case OCLASS_USER_MAPPING:
			RemoveUserMappingById(object->objectId);
			break;

		case OCLASS_EXTPROTOCOL:
			RemoveExtProtocolById(object->objectId);
			break;

		case OCLASS_COMPRESSION:
			elog(NOTICE, "dependency: not yet implemented!");
			break;
			
		default:
			elog(ERROR, "unrecognized object class: %u",
				 object->classId);
	}
}

/*
 * recordDependencyOnExpr - find expression dependencies
 *
 * This is used to find the dependencies of rules, constraint expressions,
 * etc.
 *
 * Given an expression or query in node-tree form, find all the objects
 * it refers to (tables, columns, operators, functions, etc).  Record
 * a dependency of the specified type from the given depender object
 * to each object mentioned in the expression.
 *
 * rtable is the rangetable to be used to interpret Vars with varlevelsup=0.
 * It can be NIL if no such variables are expected.
 */
void
recordDependencyOnExpr(const ObjectAddress *depender,
					   Node *expr, List *rtable,
					   DependencyType behavior)
{
	find_expr_references_context context;

	context.addrs = new_object_addresses();

	/* Set up interpretation for Vars at varlevelsup = 0 */
	context.rtables = list_make1(rtable);

	/* Scan the expression tree for referenceable objects */
	find_expr_references_walker(expr, &context);

	/* Remove any duplicates */
	eliminate_duplicate_dependencies(context.addrs);

	/* And record 'em */
	recordMultipleDependencies(depender,
							   context.addrs->refs, context.addrs->numrefs,
							   behavior);

	free_object_addresses(context.addrs);
}

/*
 * recordDependencyOnSingleRelExpr - find expression dependencies
 *
 * As above, but only one relation is expected to be referenced (with
 * varno = 1 and varlevelsup = 0).	Pass the relation OID instead of a
 * range table.  An additional frammish is that dependencies on that
 * relation (or its component columns) will be marked with 'self_behavior',
 * whereas 'behavior' is used for everything else.
 */
void
recordDependencyOnSingleRelExpr(const ObjectAddress *depender,
								Node *expr, Oid relId,
								DependencyType behavior,
								DependencyType self_behavior)
{
	find_expr_references_context context;
	RangeTblEntry rte;

	context.addrs = new_object_addresses();

	/* We gin up a rather bogus rangetable list to handle Vars */
	MemSet(&rte, 0, sizeof(rte));
	rte.type = T_RangeTblEntry;
	rte.rtekind = RTE_RELATION;
	rte.relid = relId;

	context.rtables = list_make1(list_make1(&rte));

	/* Scan the expression tree for referenceable objects */
	find_expr_references_walker(expr, &context);

	/* Remove any duplicates */
	eliminate_duplicate_dependencies(context.addrs);

	/* Separate self-dependencies if necessary */
	if (behavior != self_behavior && context.addrs->numrefs > 0)
	{
		ObjectAddresses *self_addrs;
		ObjectAddress *outobj;
		int			oldref,
					outrefs;

		self_addrs = new_object_addresses();

		outobj = context.addrs->refs;
		outrefs = 0;
		for (oldref = 0; oldref < context.addrs->numrefs; oldref++)
		{
			ObjectAddress *thisobj = context.addrs->refs + oldref;

			if (thisobj->classId == RelationRelationId &&
				thisobj->objectId == relId)
			{
				/* Move this ref into self_addrs */
				add_object_address(OCLASS_CLASS, relId, thisobj->objectSubId,
								   self_addrs);
			}
			else
			{
				/* Keep it in context.addrs */
				outobj->classId = thisobj->classId;
				outobj->objectId = thisobj->objectId;
				outobj->objectSubId = thisobj->objectSubId;
				outobj++;
				outrefs++;
			}
		}
		context.addrs->numrefs = outrefs;

		/* Record the self-dependencies */
		recordMultipleDependencies(depender,
								   self_addrs->refs, self_addrs->numrefs,
								   self_behavior);

		free_object_addresses(self_addrs);
	}

	/* Record the external dependencies */
	recordMultipleDependencies(depender,
							   context.addrs->refs, context.addrs->numrefs,
							   behavior);

	free_object_addresses(context.addrs);
}

/*
 * Recursively search an expression tree for object references.
 *
 * Note: we avoid creating references to columns of tables that participate
 * in an SQL JOIN construct, but are not actually used anywhere in the query.
 * To do so, we do not scan the joinaliasvars list of a join RTE while
 * scanning the query rangetable, but instead scan each individual entry
 * of the alias list when we find a reference to it.
 *
 * Note: in many cases we do not need to create dependencies on the datatypes
 * involved in an expression, because we'll have an indirect dependency via
 * some other object.  For instance Var nodes depend on a column which depends
 * on the datatype, and OpExpr nodes depend on the operator which depends on
 * the datatype.  However we do need a type dependency if there is no such
 * indirect dependency, as for example in Const and CoerceToDomain nodes.
 */
static bool
find_expr_references_walker(Node *node,
							find_expr_references_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		List	   *rtable;
		RangeTblEntry *rte;

		/* Find matching rtable entry, or complain if not found */
		if (var->varlevelsup >= list_length(context->rtables))
			elog(ERROR, "invalid varlevelsup %d", var->varlevelsup);
		rtable = (List *) list_nth(context->rtables, var->varlevelsup);
		if (var->varno <= 0 || var->varno > list_length(rtable))
			elog(ERROR, "invalid varno %d", var->varno);
		rte = rt_fetch(var->varno, rtable);

		/*
		 * A whole-row Var references no specific columns, so adds no new
		 * dependency.
		 */
		if (var->varattno == InvalidAttrNumber)
			return false;
		if (rte->rtekind == RTE_RELATION)
		{
			/* If it's a plain relation, reference this column */
			add_object_address(OCLASS_CLASS, rte->relid, var->varattno,
							   context->addrs);
		}
		else if (rte->rtekind == RTE_JOIN)
		{
			/* Scan join output column to add references to join inputs */
			List	   *save_rtables;

			/* We must make the context appropriate for join's level */
			save_rtables = context->rtables;
			context->rtables = list_copy_tail(context->rtables,
											  var->varlevelsup);
			if (var->varattno <= 0 ||
				var->varattno > list_length(rte->joinaliasvars))
				elog(ERROR, "invalid varattno %d", var->varattno);
			find_expr_references_walker((Node *) list_nth(rte->joinaliasvars,
														  var->varattno - 1),
										context);
			list_free(context->rtables);
			context->rtables = save_rtables;
		}
		return false;
	}
	if (IsA(node, Const))
	{
		Const	   *con = (Const *) node;
		Oid			objoid;

		/* A constant must depend on the constant's datatype */
		add_object_address(OCLASS_TYPE, con->consttype, 0,
						   context->addrs);

		/*
		 * If it's a regclass or similar literal referring to an existing
		 * object, add a reference to that object.	(Currently, only the
		 * regclass case has any likely use, but we may as well handle all the
		 * OID-alias datatypes consistently.)
		 */
		if (!con->constisnull)
		{
			switch (con->consttype)
			{
				case REGPROCOID:
				case REGPROCEDUREOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (caql_getcount(
								NULL,
								cql("SELECT COUNT(*) FROM pg_proc "
									" WHERE oid = :1 ",
									ObjectIdGetDatum(objoid))))
						add_object_address(OCLASS_PROC, objoid, 0,
										   context->addrs);
					break;
				case REGOPEROID:
				case REGOPERATOROID:
					objoid = DatumGetObjectId(con->constvalue);
					if (caql_getcount(
								NULL,
								cql("SELECT COUNT(*) FROM pg_operator "
									" WHERE oid = :1 ",
									ObjectIdGetDatum(objoid))))
						add_object_address(OCLASS_OPERATOR, objoid, 0,
										   context->addrs);
					break;
				case REGCLASSOID:
					objoid = DatumGetObjectId(con->constvalue);

					if (caql_getcount(
								NULL,
								cql("SELECT COUNT(*) FROM pg_class "
									" WHERE oid = :1 ",
									ObjectIdGetDatum(objoid))))
						add_object_address(OCLASS_CLASS, objoid, 0,
										   context->addrs);
					break;
				case REGTYPEOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (caql_getcount(
								NULL,
								cql("SELECT COUNT(*) FROM pg_type "
									" WHERE oid = :1 ",
									ObjectIdGetDatum(objoid))))
						add_object_address(OCLASS_TYPE, objoid, 0,
										   context->addrs);
					break;
			}
		}
		return false;
	}
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		/* A parameter must depend on the parameter's datatype */
		add_object_address(OCLASS_TYPE, param->paramtype, 0,
						   context->addrs);
	}
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *funcexpr = (FuncExpr *) node;

		add_object_address(OCLASS_PROC, funcexpr->funcid, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, OpExpr))
	{
		OpExpr	   *opexpr = (OpExpr *) node;

		add_object_address(OCLASS_OPERATOR, opexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, DistinctExpr))
	{
		DistinctExpr *distinctexpr = (DistinctExpr *) node;

		add_object_address(OCLASS_OPERATOR, distinctexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *opexpr = (ScalarArrayOpExpr *) node;

		add_object_address(OCLASS_OPERATOR, opexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, NullIfExpr))
	{
		NullIfExpr *nullifexpr = (NullIfExpr *) node;

		add_object_address(OCLASS_OPERATOR, nullifexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;

		add_object_address(OCLASS_PROC, aggref->aggfnoid, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, WindowRef))
	{
		WindowRef	*windowref = (WindowRef *) node;

		add_object_address(OCLASS_PROC, windowref->winfnoid, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	if (is_subplan(node))
	{
		/* Extra work needed here if we ever need this case */
		elog(ERROR, "already-planned subqueries not supported");
	}
	if (IsA(node, RelabelType))
	{
		RelabelType *relab = (RelabelType *) node;

		/* since there is no function dependency, need to depend on type */
		add_object_address(OCLASS_TYPE, relab->resulttype, 0,
						   context->addrs);
	}
	if (IsA(node, ConvertRowtypeExpr))
	{
		ConvertRowtypeExpr *cvt = (ConvertRowtypeExpr *) node;

		/* since there is no function dependency, need to depend on type */
		add_object_address(OCLASS_TYPE, cvt->resulttype, 0,
						   context->addrs);
	}
	if (IsA(node, RowExpr))
	{
		RowExpr    *rowexpr = (RowExpr *) node;

		add_object_address(OCLASS_TYPE, rowexpr->row_typeid, 0,
						   context->addrs);
	}
	if (IsA(node, RowCompareExpr))
	{
		RowCompareExpr *rcexpr = (RowCompareExpr *) node;
		ListCell   *l;

		foreach(l, rcexpr->opnos)
		{
			add_object_address(OCLASS_OPERATOR, lfirst_oid(l), 0,
							   context->addrs);
		}
		foreach(l, rcexpr->opclasses)
		{
			add_object_address(OCLASS_OPCLASS, lfirst_oid(l), 0,
							   context->addrs);
		}
		/* fall through to examine arguments */
	}
	if (IsA(node, CoerceToDomain))
	{
		CoerceToDomain *cd = (CoerceToDomain *) node;

		add_object_address(OCLASS_TYPE, cd->resulttype, 0,
						   context->addrs);
	}
	if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		Query	   *query = (Query *) node;
		ListCell   *rtable;
		bool		result;

		/*
		 * Add whole-relation refs for each plain relation mentioned in the
		 * subquery's rtable, as well as datatype refs for any datatypes used
		 * as a RECORD function's output.  (Note: query_tree_walker takes care
		 * of recursing into RTE_FUNCTION and RTE_SUBQUERY RTEs, so no need to
		 * do that here.  But keep it from looking at join alias lists.)
		 */
		foreach(rtable, query->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rtable);
			ListCell   *ct;

			switch (rte->rtekind)
			{
				case RTE_RELATION:
					add_object_address(OCLASS_CLASS, rte->relid, 0,
									   context->addrs);
					break;
				case RTE_TABLEFUNCTION:
				case RTE_FUNCTION:
					foreach(ct, rte->funccoltypes)
					{
						add_object_address(OCLASS_TYPE, lfirst_oid(ct), 0,
										   context->addrs);
					}
					break;
				default:
					break;
			}
		}

		/* Examine substructure of query */
		context->rtables = lcons(query->rtable, context->rtables);
		result = query_tree_walker(query,
								   find_expr_references_walker,
								   (void *) context,
								   QTW_IGNORE_JOINALIASES);
		context->rtables = list_delete_first(context->rtables);
		return result;
	}
	return expression_tree_walker(node, find_expr_references_walker,
								  (void *) context);
}

/*
 * Given an array of dependency references, eliminate any duplicates.
 */
static void
eliminate_duplicate_dependencies(ObjectAddresses *addrs)
{
	ObjectAddress *priorobj;
	int			oldref,
				newrefs;

	if (addrs->numrefs <= 1)
		return;					/* nothing to do */

	/* Sort the refs so that duplicates are adjacent */
	qsort((void *) addrs->refs, addrs->numrefs, sizeof(ObjectAddress),
		  object_address_comparator);

	/* Remove dups */
	priorobj = addrs->refs;
	newrefs = 1;
	for (oldref = 1; oldref < addrs->numrefs; oldref++)
	{
		ObjectAddress *thisobj = addrs->refs + oldref;

		if (priorobj->classId == thisobj->classId &&
			priorobj->objectId == thisobj->objectId)
		{
			if (priorobj->objectSubId == thisobj->objectSubId)
				continue;		/* identical, so drop thisobj */

			/*
			 * If we have a whole-object reference and a reference to a part
			 * of the same object, we don't need the whole-object reference
			 * (for example, we don't need to reference both table foo and
			 * column foo.bar).  The whole-object reference will always appear
			 * first in the sorted list.
			 */
			if (priorobj->objectSubId == 0)
			{
				/* replace whole ref with partial */
				priorobj->objectSubId = thisobj->objectSubId;
				continue;
			}
		}
		/* Not identical, so add thisobj to output set */
		priorobj++;
		priorobj->classId = thisobj->classId;
		priorobj->objectId = thisobj->objectId;
		priorobj->objectSubId = thisobj->objectSubId;
		newrefs++;
	}

	addrs->numrefs = newrefs;
}

/*
 * qsort comparator for ObjectAddress items
 */
static int
object_address_comparator(const void *a, const void *b)
{
	const ObjectAddress *obja = (const ObjectAddress *) a;
	const ObjectAddress *objb = (const ObjectAddress *) b;

	if (obja->classId < objb->classId)
		return -1;
	if (obja->classId > objb->classId)
		return 1;
	if (obja->objectId < objb->objectId)
		return -1;
	if (obja->objectId > objb->objectId)
		return 1;

	/*
	 * We sort the subId as an unsigned int so that 0 will come first. See
	 * logic in eliminate_duplicate_dependencies.
	 */
	if ((unsigned int) obja->objectSubId < (unsigned int) objb->objectSubId)
		return -1;
	if ((unsigned int) obja->objectSubId > (unsigned int) objb->objectSubId)
		return 1;
	return 0;
}

/*
 * Routines for handling an expansible array of ObjectAddress items.
 *
 * new_object_addresses: create a new ObjectAddresses array.
 */
ObjectAddresses *
new_object_addresses(void)
{
	ObjectAddresses *addrs;

	addrs = palloc(sizeof(ObjectAddresses));

	addrs->numrefs = 0;
	addrs->maxrefs = 32;
	addrs->refs = (ObjectAddress *)
		palloc(addrs->maxrefs * sizeof(ObjectAddress));

	return addrs;
}

/*
 * Add an entry to an ObjectAddresses array.
 *
 * It is convenient to specify the class by ObjectClass rather than directly
 * by catalog OID.
 */
static void
add_object_address(ObjectClass oclass, Oid objectId, int32 subId,
				   ObjectAddresses *addrs)
{
	ObjectAddress *item;

	/* enlarge array if needed */
	if (addrs->numrefs >= addrs->maxrefs)
	{
		addrs->maxrefs *= 2;
		addrs->refs = (ObjectAddress *)
			repalloc(addrs->refs, addrs->maxrefs * sizeof(ObjectAddress));
	}
	/* record this item */
	item = addrs->refs + addrs->numrefs;
	item->classId = object_classes[oclass];
	item->objectId = objectId;
	item->objectSubId = subId;
	addrs->numrefs++;
}

/*
 * Add an entry to an ObjectAddresses array.
 *
 * As above, but specify entry exactly.
 */
void
add_exact_object_address(const ObjectAddress *object,
						 ObjectAddresses *addrs)
{
	ObjectAddress *item;

	/* enlarge array if needed */
	if (addrs->numrefs >= addrs->maxrefs)
	{
		addrs->maxrefs *= 2;
		addrs->refs = (ObjectAddress *)
			repalloc(addrs->refs, addrs->maxrefs * sizeof(ObjectAddress));
	}
	/* record this item */
	item = addrs->refs + addrs->numrefs;
	*item = *object;
	addrs->numrefs++;
}

/*
 * Test whether an object is present in an ObjectAddresses array.
 *
 * We return "true" if object is a subobject of something in the array, too.
 */
bool
object_address_present(const ObjectAddress *object,
					   const ObjectAddresses *addrs)
{
	int			i;

	for (i = addrs->numrefs - 1; i >= 0; i--)
	{
		ObjectAddress *thisobj = addrs->refs + i;

		if (object->classId == thisobj->classId &&
			object->objectId == thisobj->objectId)
		{
			if (object->objectSubId == thisobj->objectSubId ||
				thisobj->objectSubId == 0)
				return true;
		}
	}

	return false;
}

/*
 * Clean up when done with an ObjectAddresses array.
 */
void
free_object_addresses(ObjectAddresses *addrs)
{
	pfree(addrs->refs);
	pfree(addrs);
}

/*
 * Determine the class of a given object identified by objectAddress.
 *
 * This function is essentially the reverse mapping for the object_classes[]
 * table.  We implement it as a function because the OIDs aren't consecutive.
 */
ObjectClass
getObjectClass(const ObjectAddress *object)
{
	switch (object->classId)
	{
		case RelationRelationId:
			/* caller must check objectSubId */
			return OCLASS_CLASS;

		case ProcedureRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_PROC;

		case TypeRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_TYPE;

		case CastRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_CAST;

		case ConstraintRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_CONSTRAINT;

		case ConversionRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_CONVERSION;

		case AttrDefaultRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_DEFAULT;

		case LanguageRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_LANGUAGE;

		case OperatorRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_OPERATOR;

		case OperatorClassRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_OPCLASS;

		case RewriteRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_REWRITE;

		case TriggerRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_TRIGGER;

		case NamespaceRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_SCHEMA;

		case AuthIdRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_ROLE;

		case DatabaseRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_DATABASE;

		case TableSpaceRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_TBLSPACE;

		case FileSpaceRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_FILESPACE;
			
		case FileSystemRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_FILESYSTEM;
			
		case ForeignDataWrapperRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_FDW;

		case ForeignServerRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_FOREIGN_SERVER;

		case UserMappingRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_USER_MAPPING;

		case ExtprotocolRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_EXTPROTOCOL;

		case CompressionRelationId:
			Assert(object->objectSubId == 0);
			return OCLASS_COMPRESSION;
	}

	/* shouldn't get here */
	elog(ERROR, "unrecognized object class: %u", object->classId);
	return OCLASS_CLASS;		/* keep compiler quiet */
}

/*
 * getObjectDescription: build an object description for messages
 *
 * The result is a palloc'd string.
 */
char *
getObjectDescription(const ObjectAddress *object)
{
	StringInfoData buffer;

	initStringInfo(&buffer);

	switch (getObjectClass(object))
	{
		case OCLASS_CLASS:
			getRelationDescription(&buffer, object->objectId);
			if (object->objectSubId != 0)
				appendStringInfo(&buffer, _(" column %s"),
								 get_relid_attribute_name(object->objectId,
													   object->objectSubId));
			break;

		case OCLASS_PROC:
			appendStringInfo(&buffer, _("function %s"),
							 format_procedure(object->objectId));
			break;

		case OCLASS_TYPE:
			appendStringInfo(&buffer, _("type %s"),
							 format_type_be(object->objectId));
			break;

		case OCLASS_CAST:
			{
				HeapTuple	tup;
				Form_pg_cast castForm;

				tup = caql_getfirst(
						NULL,
						cql("SELECT * FROM pg_cast "
							" WHERE oid = :1 ",
							ObjectIdGetDatum(object->objectId)));

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for cast %u",
						 object->objectId);

				castForm = (Form_pg_cast) GETSTRUCT(tup);

				appendStringInfo(&buffer, _("cast from %s to %s"),
								 format_type_be(castForm->castsource),
								 format_type_be(castForm->casttarget));


				break;
			}

		case OCLASS_CONSTRAINT:
			{
				HeapTuple	tup;
				Form_pg_constraint con;

				/* XXX XXX: SELECT conname, conrelid */
					
				tup = caql_getfirst(
						NULL,
						cql("SELECT * FROM pg_constraint "
							" WHERE oid = :1 ",
							ObjectIdGetDatum(object->objectId)));

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for constraint %u",
						 object->objectId);

				con = (Form_pg_constraint) GETSTRUCT(tup);

				if (OidIsValid(con->conrelid))
				{
					appendStringInfo(&buffer, _("constraint %s on "),
									 NameStr(con->conname));
					getRelationDescription(&buffer, con->conrelid);
				}
				else
				{
					appendStringInfo(&buffer, _("constraint %s"),
									 NameStr(con->conname));
				}

				break;
			}

		case OCLASS_CONVERSION:
			{
				char	   *conname;
				int			fetchCount;

				conname = caql_getcstring_plus(
						NULL,
						&fetchCount,
						NULL,
						cql("SELECT conname FROM pg_conversion "
							 " WHERE oid = :1 ",
							 ObjectIdGetDatum(object->objectId)));

				if (!fetchCount)
					elog(ERROR, "cache lookup failed for conversion %u",
						 object->objectId);
				appendStringInfo(&buffer, _("conversion %s"),
								 conname);
				pfree(conname);
				break;
			}

		case OCLASS_DEFAULT:
			{
				HeapTuple	tup;
				Form_pg_attrdef attrdef;
				ObjectAddress colobject;

				/* XXX XXX: SELECT addrelid, addnum */

				tup = caql_getfirst(
						NULL,
						cql("SELECT * FROM pg_attrdef "
							" WHERE oid = :1 ",
							ObjectIdGetDatum(object->objectId)));

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for attrdef %u",
						 object->objectId);

				attrdef = (Form_pg_attrdef) GETSTRUCT(tup);

				colobject.classId = RelationRelationId;
				colobject.objectId = attrdef->adrelid;
				colobject.objectSubId = attrdef->adnum;

				appendStringInfo(&buffer, _("default for %s"),
								 getObjectDescription(&colobject));

				break;
			}

		case OCLASS_LANGUAGE:
			{
				char	   *lanname;
				int			fetchCount;

				lanname = caql_getcstring_plus(
						NULL,
						&fetchCount,
						NULL,
						cql("SELECT lanname FROM pg_language "
							" WHERE oid = :1 ",
							ObjectIdGetDatum(object->objectId)));

				if (!fetchCount)
					elog(ERROR, "cache lookup failed for language %u",
						 object->objectId);
				appendStringInfo(&buffer, _("language %s"),
								 lanname);
				pfree(lanname);
				break;
			}

		case OCLASS_OPERATOR:
			appendStringInfo(&buffer, _("operator %s"),
							 format_operator(object->objectId));
			break;

		case OCLASS_OPCLASS:
			{
				HeapTuple	opcTup;
				Form_pg_opclass opcForm;
				char	   *nspname;
				char	   *amname;
				int			fetchCount;
				cqContext  *pcqCtx;

				pcqCtx = caql_beginscan(
						NULL,
						cql("SELECT * FROM pg_opclass "
							" WHERE oid = :1 ",
							ObjectIdGetDatum(object->objectId)));

				opcTup = caql_getnext(pcqCtx);

				if (!HeapTupleIsValid(opcTup))
					elog(ERROR, "cache lookup failed for opclass %u",
						 object->objectId);
				opcForm = (Form_pg_opclass) GETSTRUCT(opcTup);

				amname = caql_getcstring_plus(
						NULL,
						&fetchCount,
						NULL,
						cql("SELECT amname FROM pg_am "
							" WHERE oid = :1 ",
							ObjectIdGetDatum(opcForm->opcamid)));

				if (!fetchCount)
					elog(ERROR, "cache lookup failed for access method %u",
						 opcForm->opcamid);

				/* Qualify the name if not visible in search path */
				if (OpclassIsVisible(object->objectId))
					nspname = NULL;
				else
					nspname = get_namespace_name(opcForm->opcnamespace);

				appendStringInfo(&buffer, _("operator class %s for access method %s"),
								 quote_qualified_identifier(nspname,
												  NameStr(opcForm->opcname)),
								 amname);

				pfree(amname);
				caql_endscan(pcqCtx);
				break;
			}

		case OCLASS_REWRITE:
			{
				HeapTuple	tup;
				Form_pg_rewrite rule;

				/* XXX XXX: SELECT rulename, ev_class */
				tup = caql_getfirst(
						NULL,
						cql("SELECT * FROM pg_rewrite "
							" WHERE oid = :1 ",
							ObjectIdGetDatum(object->objectId)));

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for rule %u",
						 object->objectId);

				rule = (Form_pg_rewrite) GETSTRUCT(tup);

				appendStringInfo(&buffer, _("rule %s on "),
								 NameStr(rule->rulename));
				getRelationDescription(&buffer, rule->ev_class);

				break;
			}

		case OCLASS_TRIGGER:
			{
				HeapTuple	tup;
				Form_pg_trigger trig;

				/* XXX XXX: SELECT tgname, tgrelid */

				tup = caql_getfirst(
						NULL,
						cql("SELECT * FROM pg_trigger "
							" WHERE oid = :1 ",
							ObjectIdGetDatum(object->objectId)));

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for trigger %u",
						 object->objectId);

				trig = (Form_pg_trigger) GETSTRUCT(tup);

				appendStringInfo(&buffer, _("trigger %s on "),
								 NameStr(trig->tgname));
				getRelationDescription(&buffer, trig->tgrelid);

				break;
			}

		case OCLASS_SCHEMA:
			{
				char	   *nspname;

				nspname = get_namespace_name(object->objectId);
				if (!nspname)
					elog(ERROR, "cache lookup failed for namespace %u",
						 object->objectId);
				appendStringInfo(&buffer, _("schema %s"), nspname);
				break;
			}

		case OCLASS_ROLE:
			{
				appendStringInfo(&buffer, _("role %s"),
								 GetUserNameFromId(object->objectId));
				break;
			}

		case OCLASS_DATABASE:
			{
				char	   *datname;

				datname = get_database_name(object->objectId);
				if (!datname)
					elog(ERROR, "cache lookup failed for database %u",
						 object->objectId);
				appendStringInfo(&buffer, _("database %s"), datname);
				break;
			}

		case OCLASS_TBLSPACE:
			{
				char	   *tblspace;

				tblspace = get_tablespace_name(object->objectId);
				if (!tblspace)
					elog(ERROR, "cache lookup failed for tablespace %u",
						 object->objectId);
				appendStringInfo(&buffer, _("tablespace %s"), tblspace);
				break;
			}
			
		case OCLASS_FDW:
			{
				ForeignDataWrapper *fdw;

				fdw = GetForeignDataWrapper(object->objectId);
				appendStringInfo(&buffer, _("foreign-data wrapper %s"), fdw->fdwname);
				break;
			}

		case OCLASS_FOREIGN_SERVER:
			{
				ForeignServer *srv;

				srv = GetForeignServer(object->objectId);
				appendStringInfo(&buffer, _("server %s"), srv->servername);
				break;
			}

		case OCLASS_FILESPACE:
			{
				char       *fsname;

				fsname = get_filespace_name(object->objectId);
				if (!fsname)
					elog(ERROR, "cache lookup failed for filespace %u",
						 object->objectId);
				appendStringInfo(&buffer, _("filespace %s"), fsname);
				break;
			}				

		case OCLASS_FILESYSTEM:
			{
				char       *fsysname;

				fsysname = FileSystemGetNameByOid(object->objectId);
				if (!fsysname)
					elog(ERROR, "cache lookup failed for filesystem %u",
						 object->objectId);
				appendStringInfo(&buffer, _("filesystem %s"), fsysname);
				break;
			}

		case OCLASS_USER_MAPPING:
			{
				Oid			useid;
				char	   *usename;
				int			fetchCount;

				useid  = caql_getoid_plus(
						NULL,
						&fetchCount,
						NULL,
						cql("SELECT umuser FROM pg_user_mapping "
							" WHERE oid = :1 ",
							ObjectIdGetDatum(object->objectId)));

				if (!fetchCount)
					elog(ERROR, "cache lookup failed for user mapping %u",
						 object->objectId);

				if (OidIsValid(useid))
					usename = GetUserNameFromId(useid);
				else
					usename = "public";

				appendStringInfo(&buffer, _("user mapping for %s"), usename);
				break;
			}

		case OCLASS_EXTPROTOCOL:
			{
				appendStringInfo(&buffer, _("protocol %s"),
								 ExtProtocolGetNameByOid(object->objectId));
				break;
			}
		case OCLASS_COMPRESSION:
			{
				elog(NOTICE, "NOT YET IMPLEMENTED");
				break;
			}
		default:
			appendStringInfo(&buffer, "unrecognized object %u %u %d",
							 object->classId,
							 object->objectId,
							 object->objectSubId);
			break;
	}

	return buffer.data;
}

/*
 * subroutine for getObjectDescription: describe a relation
 */
static void
getRelationDescription(StringInfo buffer, Oid relid)
{
	HeapTuple	relTup;
	Form_pg_class relForm;
	char	   *nspname;
	char	   *relname;
	cqContext  *pcqCtx;

	pcqCtx = caql_beginscan(
			NULL,
			cql("SELECT * FROM pg_class "
				" WHERE oid = :1 ",
				ObjectIdGetDatum(relid)));

	relTup = caql_getnext(pcqCtx);

	if (!HeapTupleIsValid(relTup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	relForm = (Form_pg_class) GETSTRUCT(relTup);

	/* Qualify the name if not visible in search path */
	if (RelationIsVisible(relid))
		nspname = NULL;
	else
		nspname = get_namespace_name(relForm->relnamespace);

	relname = quote_qualified_identifier(nspname, NameStr(relForm->relname));

	switch (relForm->relkind)
	{
		case RELKIND_RELATION:
			if(relForm->relstorage == RELSTORAGE_AOROWS)
				appendStringInfo(buffer, _("append only row table %s"), relname);
			else if (relForm->relstorage == RELSTORAGE_PARQUET)
				appendStringInfo(buffer, _("append only parquet table %s"), relname);
			else if (relForm->relstorage == RELSTORAGE_ORC)
        appendStringInfo(buffer, _("orc table %s"), relname);
			else if (relForm->relstorage == RELSTORAGE_EXTERNAL)
    				appendStringInfo(buffer, _("external table %s"), relname);
			else
				appendStringInfo(buffer, _("table %s"), relname);
			break;
		case RELKIND_INDEX:
			appendStringInfo(buffer, _("index %s"),
							 relname);
			break;
		case RELKIND_SEQUENCE:
			appendStringInfo(buffer, _("sequence %s"),
							 relname);
			break;
		case RELKIND_UNCATALOGED:
			appendStringInfo(buffer, _("uncataloged table %s"),
							 relname);
			break;
		case RELKIND_TOASTVALUE:
			appendStringInfo(buffer, _("toast table %s"),
							 relname);
			break;
		case RELKIND_AOSEGMENTS:
			appendStringInfo(buffer, _("append only file segment listing %s"),
							 relname);
			break;
		case RELKIND_AOBLOCKDIR:
			appendStringInfo(buffer, _("append only file block directory %s"),
							 relname);
			break;
		case RELKIND_VIEW:
			appendStringInfo(buffer, _("view %s"),
							 relname);
			break;
		case RELKIND_COMPOSITE_TYPE:
			appendStringInfo(buffer, _("composite type %s"),
							 relname);
			break;
		default:
			/* shouldn't get here */
			appendStringInfo(buffer, _("relation %s"),
							 relname);
			break;
	}

	caql_endscan(pcqCtx);
}

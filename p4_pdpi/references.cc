#include "p4_pdpi/references.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gutil/proto.h"
#include "gutil/status.h"
#include "p4/v1/p4runtime.pb.h"
#include "p4_pdpi/ir.pb.h"
#include "p4_pdpi/reference_annotations.h"
#include "p4_pdpi/string_encodings/byte_string.h"

namespace pdpi {
namespace {

// Used to store a set of ConcreteFieldReferences, that partially represents a
// ConcreteTableReference, coming from an action or the match fields. Partial
// references are combined to form full references, more details below.
using PartialConcreteTableReference = absl::btree_set<ConcreteFieldReference>;

// Inherited from v1model , see `standard_metadata_t.mcast_grp`.
// https://github.com/p4lang/p4c/blob/main/p4include/v1model.p4
constexpr int kMulticastGroupIdWidth = 16;
// Inherited from v1model, see `standard_metadata_t.egress_rid`.
// https://github.com/p4lang/p4c/blob/main/p4include/v1model.p4
constexpr int kMulticastReplicaInstanceWidth = 16;

// -- Match Field Value Getters ------------------------------------------------

// Returns value of `field` in `entry`. Returns error if `entry` does not
// contain `field` or `field does not have type EXACT.
absl::StatusOr<std::string> GetMatchFieldValue(
    const IrP4MatchField& field, const p4::v1::TableEntry& entry) {
  for (const auto& match_field : entry.match()) {
    if (match_field.field_id() == field.field_id()) {
      switch (match_field.field_match_type_case()) {
        case ::p4::v1::FieldMatch::kExact: {
          return match_field.exact().value();
        }
        default: {
          ASSIGN_OR_RETURN(
              std::string type,
              gutil::GetOneOfFieldName(match_field, "field_match_type"));
          return gutil::UnimplementedErrorBuilder()
                 << "Only match field type EXACT is supported in references. "
                    "Match field '"
                 << field.field_name() << "' has type '" << type << "'.";
        }
      }
    }
  }
  return gutil::InvalidArgumentErrorBuilder()
         << "Entry is missing match field " << field.field_name();
}

// Returns value of `field` in `entry`. Returns error if `field` is not a
// multicast group entry match field.
absl::StatusOr<std::string> GetMatchFieldValue(
    const IrBuiltInMatchField& field,
    const p4::v1::MulticastGroupEntry& entry) {
  switch (field) {
    case IrBuiltInMatchField::BUILT_IN_MATCH_FIELD_MULTICAST_GROUP_ID: {
      // Built-in field multicast group id is an int in PI representation, but
      // user-defined fields involved in references encode the field as a
      // bytestring, so it is converted for the sake of equality testing.
      return BitsetToP4RuntimeByteString<kMulticastGroupIdWidth>(
          entry.multicast_group_id());
    }
    default: {
      return gutil::InternalErrorBuilder()
             << "Built-in field " << IrBuiltInMatchField_Name(field)
             << " is not a multicast group entry match field.";
    }
  }
}

// Returns value of `field` in `entry`. Returns error if `field` contains an
// `IrActionField` or is empty.
absl::StatusOr<std::string> GetMatchFieldValue(const IrMatchField& field,
                                               const p4::v1::Entity& entry) {
  switch (field.match_field_case()) {
    case IrMatchField::kP4MatchField: {
      return GetMatchFieldValue(field.p4_match_field(), entry.table_entry());
    }
    case IrMatchField::kBuiltInMatchField: {
      return GetMatchFieldValue(
          field.built_in_match_field(),
          entry.packet_replication_engine_entry().multicast_group_entry());
    }
    case IrField::FIELD_NOT_SET: {
      return gutil::InvalidArgumentErrorBuilder()
             << "IrMatchField field oneof not set.";
    }
  }
}

// -- Action Param Value Getters -----------------------------------------------

// Returns value of `field` in `entry`. Returns error if `entry` does not
// contain `field` or `field does not have type EXACT.
absl::StatusOr<std::string> GetActionFieldValue(const IrP4ActionField& field,
                                                const p4::v1::Action& action) {
  for (const auto& param : action.params()) {
    if (param.param_id() == field.parameter_id()) {
      return param.value();
    }
  }
  return gutil::InvalidArgumentErrorBuilder()
         << "Action " << field.action_name() << " is missing parameter "
         << field.parameter_name();
}

// Returns value of `field` in `entry`. Returns error if `field` is not a
// multicast group entry match field.
absl::StatusOr<std::string> GetActionFieldValue(
    const IrBuiltInActionField& field, const p4::v1::Replica& replica) {
  switch (field.parameter()) {
    case IrBuiltInParameter::BUILT_IN_PARAMETER_REPLICA_PORT: {
      return replica.port();
      break;
    }
    case IrBuiltInParameter::BUILT_IN_PARAMETER_REPLICA_INSTANCE: {
      // Built-in field instance is an int in PI representation, but
      // user-defined fields involved in references encode the field as a
      // bytestring, so it is converted for the sake of equality testing.
      return BitsetToP4RuntimeByteString<kMulticastReplicaInstanceWidth>(
          replica.instance());
    }
    default: {
      return gutil::InvalidArgumentErrorBuilder()
             << "Unknown IrBuiltInActionField " << field.DebugString();
    }
  }
}

// -- Reference Field Type -----------------------------------------------------

// Enum representing the type of reference from an `IrField` in a source entry
// to an `IrField` in a destination entry. These reference types dictate how
// ConcreteFieldReferences should be merged together to form
// ConcreteTableReferences.
//
// NOTE: There is currently no practical use case for kMatchFieldToActionParam
// and kActionParamToActionParam references. In order to avoid unnecessary
// implementation, they are unsupported and excluded from the enum. That being
// said, it would not be an impossible task to imagine how they would be
// supported. We leave this as an exercise for the reader.
enum class FieldReferenceType {
  kMatchFieldToMatchField,
  kActionParamToMatchField,
};

// Returns `FieldReferenceType` of a reference from `source` to `destination`.
// Returns error if the type of reference is not supported.
absl::StatusOr<FieldReferenceType> GetReferenceType(
    const IrField& source, const IrField& destination) {
  if (source.has_match_field() && destination.has_match_field()) {
    return FieldReferenceType::kMatchFieldToMatchField;
  } else if (source.has_action_field() && destination.has_match_field()) {
    return FieldReferenceType::kActionParamToMatchField;
  }

  return gutil::UnimplementedErrorBuilder()
         << "Unsupported field reference from IrField "
         << gutil::PrintShortTextProto(source) << " to IrField "
         << gutil::PrintShortTextProto(destination);
}

// -- Validation function ------------------------------------------------------

absl::Status ValidateEntityBelongsToTable(const p4::v1::Entity& entity,
                                          const IrTable& table) {
  switch (entity.entity_case()) {
    case p4::v1::Entity::kTableEntry: {
      if (!table.has_p4_table()) {
        return gutil::InvalidArgumentErrorBuilder()
               << "Entity contained a table entry but IrTable was not an "
                  "IrP4Table. IrTable: "
               << table.DebugString();
      }
      if (entity.table_entry().table_id() != table.p4_table().table_id()) {
        return gutil::InvalidArgumentErrorBuilder()
               << "Provided table entry had id "
               << entity.table_entry().table_id() << " but IrP4Table had id "
               << table.p4_table().table_id();
      }
      return absl::OkStatus();
    }
    case p4::v1::Entity::kPacketReplicationEngineEntry: {
      if (!entity.packet_replication_engine_entry()
               .has_multicast_group_entry()) {
        return gutil::UnimplementedErrorBuilder()
               << "Only packet replication engine entries of type "
                  "multicast are supported. Entity: "
               << entity.DebugString();
      }
      if (!table.has_built_in_table()) {
        return gutil::InvalidArgumentErrorBuilder()
               << "Entity contained built-in table but IrTable was "
                  "not a IrBuiltInTable. IrTable: "
               << table.DebugString();
      }
      if (table.built_in_table() != BUILT_IN_TABLE_MULTICAST_GROUP_TABLE) {
        return gutil::InvalidArgumentErrorBuilder()
               << "Entity contained multicast group entry but IrBuiltInTable "
                  "was not BUILT_IN_TABLE_MULTICAST_GROUP_ENTRY. IrTable: "
               << table.DebugString();
      }
      return absl::OkStatus();
    }
    default: {
      ASSIGN_OR_RETURN(std::string type,
                       gutil::GetOneOfFieldName(entity, "entity"));
      return gutil::UnimplementedErrorBuilder()
             << "Cannot create reference entries for unsupported entity type: "
             << type << ". Entity: " << entity.DebugString();
    }
  }
}

absl::StatusOr<PartialConcreteTableReference> GetPartialReferenceFromAction(
    std::vector<const IrTableReference::FieldReference*>
        action_field_to_match_field_references,
    const p4::v1::Action& action) {
  PartialConcreteTableReference result;
  for (const auto& reference : action_field_to_match_field_references) {
    ASSIGN_OR_RETURN(std::string source_name,
                     GetNameOfField(reference->source()));
    ASSIGN_OR_RETURN(std::string destination_name,
                     GetNameOfField(reference->destination()));
    ASSIGN_OR_RETURN(
        std::string param_value,
        GetActionFieldValue(
            reference->source().action_field().p4_action_field(), action));

    result.insert(ConcreteFieldReference{
        .source_field = std::move(source_name),
        .destination_field = std::move(destination_name),
        .value = std::move(param_value),
    });
  }
  return result;
}

absl::StatusOr<PartialConcreteTableReference> GetPartialReferenceFromReplica(
    std::vector<const IrTableReference::FieldReference*>
        replica_field_to_match_field_references,
    const p4::v1::Replica& replica) {
  PartialConcreteTableReference partial_reference;
  for (const auto& reference : replica_field_to_match_field_references) {
    ASSIGN_OR_RETURN(std::string source_name,
                     GetNameOfField(reference->source()));
    ASSIGN_OR_RETURN(std::string destination_name,
                     GetNameOfField(reference->destination()));

    ASSIGN_OR_RETURN(
        std::string param_value,
        GetActionFieldValue(
            reference->source().action_field().built_in_action_field(),
            replica));

    partial_reference.insert(ConcreteFieldReference{
        .source_field = std::move(source_name),
        .destination_field = std::move(destination_name),
        .value = std::move(param_value),
    });
  }
  return partial_reference;
}

}  // namespace

absl::StatusOr<absl::flat_hash_set<ConcreteTableReference>>
OutgoingConcreteTableReferences(const IrTableReference& reference_info,
                                const ::p4::v1::Entity& entity) {
  RETURN_IF_ERROR(
      ValidateEntityBelongsToTable(entity, reference_info.source_table()));

  // The set of outgoing ConcreteTableReferences is created by taking the union
  // of each action partial reference with the match field partial reference.
  // If there are no action partial reference, then the match field partial
  // reference alone will form a single ConcreteTableReference.

  // STEP 1: Group references by match field or source action.
  std::vector<const IrTableReference::FieldReference*>
      match_field_to_match_field_references;
  absl::flat_hash_map<int, std::vector<const IrTableReference::FieldReference*>>
      action_field_to_match_field_references_by_action_id;
  // Built-in actions do not have an action id so references from a built-in
  // action are explicitly stored in separate containers.
  std::vector<const IrTableReference::FieldReference*>
      built_in_replica_field_to_match_field_references;
  for (const auto& field_reference : reference_info.field_references()) {
    ASSIGN_OR_RETURN(FieldReferenceType reference_type,
                     GetReferenceType(field_reference.source(),
                                      field_reference.destination()));
    switch (reference_type) {
      case FieldReferenceType::kMatchFieldToMatchField: {
        match_field_to_match_field_references.push_back(&field_reference);
        break;
      }
      case FieldReferenceType::kActionParamToMatchField: {
        if (field_reference.source().action_field().has_p4_action_field()) {
          action_field_to_match_field_references_by_action_id
              [field_reference.source()
                   .action_field()
                   .p4_action_field()
                   .action_id()]
                  .push_back(&field_reference);
        } else {
          built_in_replica_field_to_match_field_references.push_back(
              &field_reference);
        }
        break;
      }
    }
  }

  // STEP 2: Get partial references for each action.
  std::vector<PartialConcreteTableReference> action_partial_references;
  switch (entity.entity_case()) {
    case p4::v1::Entity::kTableEntry: {
      const p4::v1::TableEntry& entry = entity.table_entry();
      switch (entry.action().type_case()) {
        case p4::v1::TableAction::kAction: {
          ASSIGN_OR_RETURN(
              action_partial_references.emplace_back(),
              GetPartialReferenceFromAction(
                  action_field_to_match_field_references_by_action_id
                      [entry.action().action().action_id()],
                  entry.action().action()));
          break;
        }
        case p4::v1::TableAction::kActionProfileActionSet: {
          for (const auto& action : entry.action()
                                        .action_profile_action_set()
                                        .action_profile_actions()) {
            ASSIGN_OR_RETURN(
                action_partial_references.emplace_back(),
                GetPartialReferenceFromAction(
                    action_field_to_match_field_references_by_action_id
                        [action.action().action_id()],
                    action.action()));
          }
          break;
        }
        default: {
          return gutil::InvalidArgumentErrorBuilder()
                 << "Unknown TableAction " << entry.action().DebugString();
        }
      }
      break;
    }
    case p4::v1::Entity::kPacketReplicationEngineEntry: {
      const p4::v1::MulticastGroupEntry& entry =
          entity.packet_replication_engine_entry().multicast_group_entry();
      for (const auto& replica : entry.replicas()) {
        ASSIGN_OR_RETURN(
            action_partial_references.emplace_back(),
            GetPartialReferenceFromReplica(
                built_in_replica_field_to_match_field_references, replica));
      }
      break;
    }
    default: {
      return gutil::InvalidArgumentErrorBuilder()
             << "Unknown Entity " << entity.DebugString();
    }
  }

  // Step 3: Get partial reference for the match fields.
  PartialConcreteTableReference match_field_partial_reference;
  for (const auto& match_field_reference :
       match_field_to_match_field_references) {
    ASSIGN_OR_RETURN(std::string source_name,
                     GetNameOfField(match_field_reference->source()));
    ASSIGN_OR_RETURN(std::string destination_name,
                     GetNameOfField(match_field_reference->destination()));
    ASSIGN_OR_RETURN(
        std::string match_field_value,
        GetMatchFieldValue(match_field_reference->source().match_field(),
                           entity));
    match_field_partial_reference.insert(ConcreteFieldReference{
        .source_field = std::move(source_name),
        .destination_field = std::move(destination_name),
        .value = std::move(match_field_value),
    });
  }

  // Step 4: Combine partial concrete table references from actions and match
  // fields.
  ASSIGN_OR_RETURN(std::string source_table_name,
                   GetNameOfTable(reference_info.source_table()));
  ASSIGN_OR_RETURN(std::string destination_table_name,
                   GetNameOfTable(reference_info.destination_table()));

  absl::flat_hash_set<ConcreteTableReference> result;
  // If `entity` only refers to another table via match fields, it should only
  // have a single ConcreteTableReference.
  if (action_partial_references.empty() &&
      !match_field_partial_reference.empty()) {
    result.insert(ConcreteTableReference{
        .source_table = source_table_name,
        .destination_table = destination_table_name,
        .fields = std::move(match_field_partial_reference),
    });
  } else {
    // Union the partial references from actions (if any) with the partial
    // reference from match fields (if any).
    for (auto& partial_reference : action_partial_references) {
      partial_reference.insert(match_field_partial_reference.begin(),
                               match_field_partial_reference.end());

      // Don't create empty references.
      if (partial_reference.empty()) continue;

      result.insert(ConcreteTableReference{
          .source_table = source_table_name,
          .destination_table = destination_table_name,
          .fields = std::move(partial_reference),
      });
    }
  }
  return result;
}

absl::StatusOr<absl::flat_hash_set<ConcreteTableReference>>
PossibleIncomingConcreteTableReferences(const IrTableReference& reference_info,
                                        const ::p4::v1::Entity& entity) {
  RETURN_IF_ERROR(
      ValidateEntityBelongsToTable(entity, reference_info.destination_table()));

  // The set of possible incoming ConcreteTableReferences is created by taking
  // the union of each partial reference coming from an action with the partial
  // reference coming from match fields. In addition, the partial
  // reference coming from match fields also creates its own reference since
  // referencing entities may contain no action.

  // STEP 1: Group references by match field or source action.
  std::vector<const IrTableReference::FieldReference*>
      match_field_to_match_field_references;
  absl::flat_hash_map<std::string,
                      std::vector<const IrTableReference::FieldReference*>>
      action_field_to_match_field_references_by_action_name;
  // NOTE: We do not need a container specific for built-in actions because we
  // are not accessing the actions in `entity`. We are only stating that a match
  // field is being referenced by some action, and action names are sufficient
  // for that purpose, which both built-ins and user-defined support.

  for (const auto& field_reference : reference_info.field_references()) {
    ASSIGN_OR_RETURN(FieldReferenceType reference_type,
                     GetReferenceType(field_reference.source(),
                                      field_reference.destination()));
    switch (reference_type) {
      case FieldReferenceType::kMatchFieldToMatchField: {
        match_field_to_match_field_references.push_back(&field_reference);
        break;
      }
      case FieldReferenceType::kActionParamToMatchField: {
        ASSIGN_OR_RETURN(
            std::string action_name,
            GetNameOfAction(field_reference.source().action_field()));
        action_field_to_match_field_references_by_action_name[action_name]
            .push_back(&field_reference);
        break;
      }
    }
  }

  // STEP 2: Get partial reference for each possible source action.
  std::vector<PartialConcreteTableReference> action_partial_references;
  for (const auto& [action_name, references] :
       action_field_to_match_field_references_by_action_name) {
    PartialConcreteTableReference partial_reference;
    for (const auto& reference : references) {
      ASSIGN_OR_RETURN(std::string source_name,
                       GetNameOfField(reference->source()));
      ASSIGN_OR_RETURN(std::string destination_name,
                       GetNameOfField(reference->destination()));
      ASSIGN_OR_RETURN(
          std::string match_field_value,
          GetMatchFieldValue(reference->destination().match_field(), entity));
      partial_reference.insert(ConcreteFieldReference{
          .source_field = source_name,
          .destination_field = destination_name,
          .value = match_field_value,
      });
    }
    action_partial_references.push_back(std::move(partial_reference));
  }

  // Step 3: Get partial reference for the match fields.
  PartialConcreteTableReference match_field_partial_reference;
  for (const auto& match_field_reference :
       match_field_to_match_field_references) {
    ASSIGN_OR_RETURN(std::string source_name,
                     GetNameOfField(match_field_reference->source()));
    ASSIGN_OR_RETURN(std::string destination_name,
                     GetNameOfField(match_field_reference->destination()));
    ASSIGN_OR_RETURN(
        std::string match_field_value,
        GetMatchFieldValue(match_field_reference->destination().match_field(),
                           entity));
    match_field_partial_reference.insert(ConcreteFieldReference{
        .source_field = source_name,
        .destination_field = destination_name,
        .value = match_field_value,
    });
  }

  // Step 4: Combine partial concrete table references from actions and match
  // fields.
  ASSIGN_OR_RETURN(std::string source_table_name,
                   GetNameOfTable(reference_info.source_table()));
  ASSIGN_OR_RETURN(std::string destination_table_name,
                   GetNameOfTable(reference_info.destination_table()));

  absl::flat_hash_set<ConcreteTableReference> result;
  // If `entity` is only referenced by another table via match fields, it should
  // a single ConcreteTableReference accounting for this.
  if (!match_field_partial_reference.empty()) {
    result.insert(ConcreteTableReference{
        .source_table = source_table_name,
        .destination_table = destination_table_name,
        .fields = match_field_partial_reference,
    });
  }

  // Union the partial references from actions (if any) with the partial
  // reference from match fields (if any).
  for (auto& partial_reference : action_partial_references) {
    partial_reference.insert(match_field_partial_reference.begin(),
                             match_field_partial_reference.end());

    // Don't create empty references.
    if (partial_reference.empty()) continue;

    result.insert(ConcreteTableReference{
        .source_table = source_table_name,
        .destination_table = destination_table_name,
        .fields = std::move(partial_reference),
    });
  }

  return result;
}

// ConcreteFieldReference operators
bool operator==(const ConcreteFieldReference& lhs,
                const ConcreteFieldReference& rhs) {
  return lhs.source_field == rhs.source_field &&
         lhs.destination_field == rhs.destination_field &&
         lhs.value == rhs.value;
}
bool operator!=(const ConcreteFieldReference& lhs,
                const ConcreteFieldReference& rhs) {
  return !(lhs == rhs);
}
bool operator<(const ConcreteFieldReference& lhs,
               const ConcreteFieldReference& rhs) {
  if (lhs.source_field != rhs.source_field) {
    return lhs.source_field < rhs.source_field;
  }
  if (lhs.destination_field != rhs.destination_field) {
    return lhs.destination_field < rhs.destination_field;
  }
  return lhs.value < rhs.value;
}

// ConcreteTableReference operators
bool operator==(const ConcreteTableReference& lhs,
                const ConcreteTableReference& rhs) {
  return lhs.source_table == rhs.source_table &&
         lhs.destination_table == rhs.destination_table &&
         lhs.fields == rhs.fields;
}

bool operator!=(const ConcreteTableReference& lhs,
                const ConcreteTableReference& rhs) {
  return !(lhs == rhs);
}

bool operator<(const ConcreteTableReference& lhs,
               const ConcreteTableReference& rhs) {
  if (lhs.source_table != rhs.source_table) {
    return lhs.source_table < rhs.source_table;
  }
  if (lhs.destination_table != rhs.destination_table) {
    return lhs.destination_table < rhs.destination_table;
  }
  return lhs.fields < rhs.fields;
}

}  // namespace pdpi

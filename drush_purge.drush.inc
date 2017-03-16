<?php

/**
 * @file
 * purge entities Drush command
 */

/**
 * Implementation of hook_drush_command().
 */
function drush_purge_drush_command() {
  $items = array();
  $items['purge-content'] = array(
    'callback' => '_drush_purge',
    'description' => dt('Delete all nodes, nodes of a specific content type or users.'),
    'arguments' => array(
      'entity_type' => dt('An active entiity type')
    ),
    'options' => array(
      'keep' => dt('Indicate number or a date string to indicate how much content to keep'),
      'bundle' => dt('Bundle in a selected entity type')
    ),
    'examples' => array(
      'drush purge-content node --keep="-30 days"' => 'Retain any nodes published in the last 30 days',
      'drush purge-content file --keep=200' => 'Retain 200 files',
      'drush purge-content node --keep="1/1/2015"' => 'Retain any nodes published after 1/1/2015',
      'drush purge-content node --bundle="basic_page"' => 'Delete all basic page nodes'
     ),
    'aliases' => array('purge'),
  );
  return $items;
}

/**
 * Implementation of hook_drush_help().
 */
function drush_purge_drush_help($section) {
  switch ($section) {
    case 'drush:purge-content':
    return dt("Delete all nodes, nodes of a specific content type or users.");
  }
}

/**
 * Drush callback to delete content
 */
function _drush_purge($entity_type = null) {

  //Get info for all entities to collect entity types
  $entity_info = field_info_bundles();
  $entity_types = array_keys($entity_info);

  //If the entity type isn't applicable or wasn't provided prompt the user
  if(!in_array($entity_type, $entity_types)) {
    //Key the array by values
    $choices = array_combine($entity_types, $entity_types);
    //Set entity type if selected or abort
    $entity_type = drush_choice($choices, dt("Choose an applicable entity type:"));
    if(empty($entity_type)) {
      return drush_user_abort();
    }
  }

  //Set supported entity types
  $supported = array('node', 'file');
  //If the entity type is supported process the request otherwise abort.
  if(!in_array($entity_type, $supported)) {
    $message = dt(
      "@type entities are currently not supported; only the following are: @supported",
      array(
        '@type' => $entity_type,
        '@supported' => implode($supported, ', ')
      )
    );
    drush_print($message);
    return drush_user_abort();
  }

  //Check if bundle has been provided and set
  $bundle = drush_get_option('bundle');

  //Get info for selected entity_type to collect bundles
  $bundle_info = field_info_bundles($entity_type);
  $bundles = array_keys($bundle_info);
  //If the bundle isn't applicable or wasn't provided prompt the user
  if(!in_array($bundle, $bundles)) {
    $choices = array_combine($bundles, $bundles);
    $choices = array_merge(array('all' => 'all'), $choices);
    $bundle = drush_choice($choices, dt("Choose an applicable bundle:"));
    if(empty($bundle)) {
      return drush_user_abort();
    } elseif ($bundle == 'all') {
      //Set bundle to null so the query is not filtered downstream
      $bundle = null;
    }
  }

  //Get keep option and prompt user for it if not provided
  $keep = drush_get_option('keep');
  if (!$keep) {
    $prompt = dt('Enter a date string or interger indicating what you want to keep');
    $keep = drush_prompt($prompt);
  }

  //Validate keep data; aborts function if invalid
  $keep_type = _keep_data_validate($keep);
  if(!$keep_type) {
    drush_print('The date or interger you entered is not valid.');
    return drush_user_abort();
  }

  //If we make it here we're ready to process!
  _drush_purge_process($entity_type, $keep, $keep_type, $bundle);
}

/**
 * Returns type of data if valid and false otherwise
 */
function _keep_data_validate($keep) {
  if (ctype_digit($keep)) {
    return 'int';
  } elseif (strtotime($keep)) {
    return 'date';
  } else {
    return false;
  }
}

/**
 * Selects data based on user input and processes it via the Batch API
 */
function _drush_purge_process($entity_type, $keep, $keep_type, $bundle = null){
  //Map the order property for each supported type
  $type_order = array(
    'node' => 'created',
    'file' => 'timestamp'
  );
  //Create query to count total entities based on type and bundle (if set)
  $cquery = new EntityFieldQuery();
  $cquery->entityCondition('entity_type', $entity_type);
  if($bundle) {
    $cquery->entityCondition('bundle', $bundle);
  }
  //Set total items
  $total = $cquery->count()->execute();

  //Create query to select entities for processing
  $query = new EntityFieldQuery();
  $query->entityCondition('entity_type', $entity_type);
  if($bundle) {
    $query->entityCondition('bundle', $bundle);
  }
  //Sort so newer entities are retained
  $query->propertyOrderBy($type_order[$entity_type], 'DESC');
  //Alter query based on keep_type
  if($keep_type == 'date') {
    //If we're processing a date select everything older than what's provided
    $query->propertyCondition($type_order[$entity_type], strtotime($keep), '<=');
  } elseif($keep_type == 'int') {
    //If we're processing an int select evertyhing else outside provided range
    $query->range($keep, $total);
  }
  $result = $query->execute();

  //If there's data to process move forward, otherwise abort
  if (isset($result[$entity_type])) {
    $ids = array_keys($result[$entity_type]);
    //Calculate items to process
    $pcount = count($ids);
    //Prompt user with details before continuing
    $prompt = dt(
      'Ready to delete @pcount of @total @entity_info?',
      array(
        '@pcount' => $pcount,
        '@entity_info' => ($bundle ? "{$bundle} " : '') . "{$entity_type}s",
        '@total' => $total
      )
    );
    $confirm = drush_confirm($prompt);
    //Abort if user doesn't confirm
    if(!$confirm) {
      return drush_user_abort();
    }

    //Break up all of our data so each process does not time out.
    $chunks = array_chunk($ids, 20);
    $operations = array();
    $count_chunks = count($chunks);

    //For every chunk, assign some method to run on that chunk of data
    foreach ($chunks as $chunk) {
      $i++;
      $pcount = $pcount-count($chunk);
      $operations[] = array(
        '_drush_purge_batch_delete',
        array(
          $chunk,
          $entity_type,
          'details'=> dt(
            'Deleting chunk @chunk  of @total_chunks (@remaining @entity_types remaining in queue)',
            array(
              '@chunk '=> $i,
              '@total_chunks'=> $count_chunks,
              '@entity_type' => $entity_type,
              '@remaining' => $pcount,
            )
          )
        )
      );

    }

    //Put all that information into our batch array
    $batch = array(
      'operations' => $operations,
      'title' => dt('Import batch'),
      'init_message' => dt('Initializing'),
      'error_message' => dt('An error occurred'),
      'finished' => '_drush_purge_batch_finished'
    );

    //Get the batch process all ready!
    batch_set($batch);
    $batch =& batch_get();

    //Because we are doing this on the back-end, we set progressive to false.
    $batch['progressive'] = FALSE;

    //Start processing the batch operations.
    drush_backend_batch_process();

  } else {
    //Abort function if the query returned no results
    $message = dt('No content to delete.');
    drush_print($message);
    return drush_user_abort();
  }
}

/**
 * Processing function called by the Batch API
 */
function _drush_purge_batch_delete($chunk, $entity_type, $operation_details, &$context) {

  //Without contrib there is no standard method of deleting an entity!
  switch($entity_type) {
    case "node":
      node_delete_multiple($chunk);
      break;
    case "file":
      foreach($chunk as $id) {
        $file = file_load($id);
        file_delete($file, true);
      }
      break;
  }
  //Will show what chunk we're on.
  $context['message'] = $operation_details;
}

/**
 * Function called by the Batch API when complete
 */
function _drush_purge_batch_finished($success, $results, $operations) {
  //Let the user know we have finished!
  $message = dt('Finished deleting!');
  drush_print($message);
}
